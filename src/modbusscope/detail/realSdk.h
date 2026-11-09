#pragma once
#include <memory>

#include "Isdk.h"

namespace modbusscope::detail {

// Production ISdk: a dedicated QThread owns QModbusTcpClient and its event
// loop. No Qt type appears in this header.
class RealSdk final : public ISdk {
public:
    explicit RealSdk(std::uint32_t requestTimeoutMs);
    ~RealSdk() override;

    bool start(std::string *error) override;
    ErrorCode connect(const TcpEndpoint &endpoint, std::uint32_t timeoutMs,
                      std::string *detail) override;
    ErrorCode disconnect(std::uint32_t timeoutMs, std::string *detail) override;
    ErrorCode read(const ReadRequest &request, std::uint32_t timeoutMs,
                   ReadResponse *response, std::string *detail) override;
    ErrorCode write(const WriteRequest &request, std::uint32_t timeoutMs,
                    WriteResponse *response, std::string *detail) override;
    LifeState state() const override;
    void stop() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace modbusscope::detail
