#pragma once
#include <cstdint>
#include <string>

#include "modbusscope/config.h"
#include "modbusscope/error.h"
#include "modbusscope/state.h"
#include "modbusscope/request.h"

namespace modbusscope::detail {

// Internal synchronous seam over the transport. Implementations are not
// required to be thread-safe; Client::Impl serializes every call.
class ISdk {
public:
    virtual ~ISdk() = default;

    // Starts the transport (thread/event loop). Returns false and fills
    // *error on failure.
    virtual bool start(std::string *error) = 0;

    virtual ErrorCode connect(const TcpEndpoint &endpoint,
                              std::uint32_t timeoutMs,
                              std::string *detail) = 0;
    virtual ErrorCode disconnect(std::uint32_t timeoutMs,
                                 std::string *detail) = 0;

    // On success *response is fully populated; on failure it is untouched.
    virtual ErrorCode read(const ReadRequest &request,
                           std::uint32_t timeoutMs,
                           ReadResponse *response,
                           std::string *detail) = 0;
    virtual ErrorCode write(const WriteRequest &request,
                            std::uint32_t timeoutMs,
                            WriteResponse *response,
                            std::string *detail) = 0;

    // Snapshot of the underlying device state. May block on the transport
    // thread; do not call it while holding the transport call lock.
    virtual LifeState state() const = 0;

    // Stops the transport and joins its thread. Idempotent, noexcept.
    virtual void stop() noexcept = 0;
};

} // namespace modbusscope::detail
