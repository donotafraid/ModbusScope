#include "modbusscope/error.h"
#include "modbusscope/state.h"

#include <string>

namespace modbusscope {
namespace {

const char *message_for(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:                 return "Ok";
    case ErrorCode::InvalidArgument:    return "Invalid argument";
    case ErrorCode::NotConnected:       return "Not connected";
    case ErrorCode::Timeout:            return "Timeout";
    case ErrorCode::ProtocolError:      return "Modbus protocol error";
    case ErrorCode::ConnectionFailed:   return "Connection failed";
    case ErrorCode::Closed:             return "Client is closed";
    case ErrorCode::Internal:           return "Internal error";
    }
    return "Unknown error";
}

class ClientErrorCategory final : public std::error_category {
public:
    const char *name() const noexcept override { return "modbusscope"; }

    std::string message(int value) const override {
        return message_for(static_cast<ErrorCode>(value));
    }
};

} // namespace

std::error_code make_error_code(ErrorCode code) noexcept {
    static const ClientErrorCategory category;
    return {static_cast<int>(code), category};
}

Error::Error() noexcept : m_code(make_error_code(ErrorCode::Ok)) {}

Error::Error(ErrorCode code) noexcept : m_code(make_error_code(code)) {}

Error::Error(std::error_code code) noexcept : m_code(std::move(code)) {}

ErrorCode Error::value() const noexcept {
    return static_cast<ErrorCode>(m_code.value());
}

std::error_code Error::code() const noexcept { return m_code; }

const char *Error::message() const noexcept {
    // Return a static string; std::error_code::message() allocates.
    return message_for(value());
}

const char *to_string(LifeState state) noexcept {
    switch (state) {
    case LifeState::IDLE:       return "IDLE";
    case LifeState::CONNECTING: return "CONNECTING";
    case LifeState::READY:      return "READY";
    case LifeState::RECOVERING: return "RECOVERING";
    case LifeState::GIVEN_UP:   return "GIVEN_UP";
    case LifeState::CLOSED:     return "CLOSED";
    }
    return "UNKNOWN";
}

} // namespace modbusscope
