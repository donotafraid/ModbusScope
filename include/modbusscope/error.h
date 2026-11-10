#pragma once
#include <system_error>

namespace modbusscope {

// Internal transport error code.
//
// This is the vocabulary of detail::ISdk, not the public API. Callers receive
// RichError through Client; modbusscope.h does not include this header. Keep the
// two layers separate so the public surface cannot grow a second error model.
//
// Ok is only used as the default Error value; a successful Result never carries
// ErrorCode::Ok.
enum class ErrorCode : int {
    Ok = 0,
    InvalidArgument,       // 参数越界/非法功能码/空 host
    NotConnected,          // 状态非 Ready 时发起 read
    Timeout,               // 连接或请求超时
    ProtocolError,         // Modbus 异常响应 / 帧格式错误
    ConnectionFailed,      // TCP 建立失败/拒绝/reset
    Closed,                // 终态 Closed 上调用
    Internal,              // 内存分配失败等内部错误
};

// 轻量错误包装：只持有 std::error_code，不分配、不抛异常。
class Error {
public:
    Error() noexcept;                         // 等价于 Ok
    explicit Error(ErrorCode code) noexcept;
    explicit Error(std::error_code code) noexcept;

    ErrorCode value() const noexcept;
    std::error_code code() const noexcept;
    const char* message() const noexcept;     // 静态字符串，不分配

private:
    std::error_code m_code;
};

std::error_code make_error_code(ErrorCode code) noexcept;

} // namespace modbusscope

namespace std {
template <> struct is_error_code_enum<modbusscope::ErrorCode> : true_type {};
}