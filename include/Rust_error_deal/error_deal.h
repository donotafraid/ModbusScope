#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

class RichError {
public:
    enum class ErrorCode : uint8_t {
        // ---- 状态机非法 ----
        INVALID_STATE = 0,
        RECREATING,
        GIVENUP_ING,
        NOT_GIVEN_UP,
        ALREADY_TERMINATED,
        NOT_INITIALIZED,

        // ---- 参数 / 配置 ----
        INVALID_ARGUMENT,
        INVALID_CONFIG,
        TYPE_MISMATCH,
        TYPE_NOTSUPPORT,
        CONVERSION_FAILED,

        // ---- 连接 / 会话 ----
        NOT_CONNECTED,
        CONNECT_TIMEOUT,
        SESSION_NOT_ACTIVATED,
        CHANNEL_NOT_OPEN,
        CHANNEL_NOT_CLOSED,
        DISCONNECT_TIMEOUT,

        // ---- 并发 / 资源 ----
        THREAD_BUSY,
        IN_FLIGHT_TIMEOUT,
        BUDGET_EXCEEDED,

        // ---- SDK / 服务 ----
        SERVICE_FAILED,
        SERVICE_RETRY_EXHAUSTED,
        SDK_ERROR,

        // ---- 内部 ----
        INTERNAL_ERROR,
        UNKNOWN,
        OK
    };

    RichError() = default;
    RichError(ErrorCode code, std::string context)
        : code_(code), context_(std::move(context)) {}

    // 兼容旧调用：只有 context 时归为 UNKNOWN
    explicit RichError(std::string context)
        : code_(ErrorCode::UNKNOWN), context_(std::move(context)) {}

    ErrorCode   code()    const noexcept { return code_; }
    const std::string& context() const noexcept { return context_; }

    const char* what() const noexcept { return context_.c_str(); }

    // 便于日志/断言输出
    const char* codeName() const noexcept {
        switch (code_) {
            case ErrorCode::INVALID_STATE:          return "INVALID_STATE";
            case ErrorCode::RECREATING:             return "RECREATING";
            case ErrorCode::NOT_GIVEN_UP:           return "NOT_GIVEN_UP";
            case ErrorCode::GIVENUP_ING:            return "GIVENUP_ING";
            case ErrorCode::ALREADY_TERMINATED:     return "ALREADY_TERMINATED";
            case ErrorCode::NOT_INITIALIZED:        return "NOT_INITIALIZED";
            case ErrorCode::INVALID_ARGUMENT:       return "INVALID_ARGUMENT";
            case ErrorCode::INVALID_CONFIG:         return "INVALID_CONFIG";
            case ErrorCode::TYPE_MISMATCH:          return "TYPE_MISMATCH";
            case ErrorCode::TYPE_NOTSUPPORT:        return "TYPE_NOTSUPPORT";
            case ErrorCode::CONVERSION_FAILED:      return "CONVERSION_FAILED";
            case ErrorCode::NOT_CONNECTED:          return "NOT_CONNECTED";
            case ErrorCode::CONNECT_TIMEOUT:        return "CONNECT_TIMEOUT";
            case ErrorCode::SESSION_NOT_ACTIVATED:  return "SESSION_NOT_ACTIVATED";
            case ErrorCode::CHANNEL_NOT_OPEN:       return "CHANNEL_NOT_OPEN";
            case ErrorCode::CHANNEL_NOT_CLOSED:     return "CHANNEL_NOT_CLOSED";
            case ErrorCode::DISCONNECT_TIMEOUT:     return "DISCONNECT_TIMEOUT";
            case ErrorCode::THREAD_BUSY:            return "THREAD_BUSY";
            case ErrorCode::IN_FLIGHT_TIMEOUT:      return "IN_FLIGHT_TIMEOUT";
            case ErrorCode::BUDGET_EXCEEDED:        return "BUDGET_EXCEEDED";
            case ErrorCode::SERVICE_FAILED:         return "SERVICE_FAILED";
            case ErrorCode::SERVICE_RETRY_EXHAUSTED:return "SERVICE_RETRY_EXHAUSTED";
            case ErrorCode::SDK_ERROR:              return "SDK_ERROR";
            case ErrorCode::INTERNAL_ERROR:         return "INTERNAL_ERROR";
            case ErrorCode::UNKNOWN:                return "UNKNOWN";
            case ErrorCode::OK:                     return "OK";
        }
        return "UNKNOWN";
    }

private:
    ErrorCode   code_{ErrorCode::UNKNOWN};
    std::string context_;
};

struct Unit {};

struct SuccessTag {};
struct ErrorTag {};

template <typename T, typename E> class Result {
private:
  //  member variable
  std::variant<T, E> result;

public:
  using ValueType = T;
  using ErrorType = E;

  template <typename U> struct is_result_type : std::false_type {};
  template <typename U, typename F>
  struct is_result_type<Result<U, F>> : std::true_type {};

  Result(SuccessTag,const T &success) : result(success) {}
  Result(SuccessTag,T &&success) : result(std::move(success)) {}
  Result(ErrorTag,const E &fail) : result(fail) {}
  Result(ErrorTag,E &&fail) : result(std::move(fail)) {}

  // 辅助工厂函数
  static Result success(T value) {
    return Result(SuccessTag{}, std::move(value));
  }

  static Result error(E error) {
    return Result(ErrorTag{}, std::move(error));
  }

  //  check result type function
  constexpr bool is_success() const {
    /*
    在const成员函数内部：

    不能修改类的成员变量（除非是mutable的）

    只能调用其他const成员函数
    */
    return std::holds_alternative<T>(result);
  }

  constexpr bool is_fail() const { return std::holds_alternative<E>(result); }

  constexpr explicit operator bool() const noexcept { return is_success(); }
  constexpr bool has_value() const noexcept { return is_success(); }
  constexpr bool has_error() const noexcept { return is_fail(); }

  constexpr T *get() & { return is_success() ? &std::get<T>(result) : nullptr; }
  constexpr const T *get() const & {
    return is_success() ? &std::get<T>(result) : nullptr;
  }
  constexpr E *get_error() & {
    return is_fail() ? &std::get<E>(result) : nullptr;
  }
  constexpr const E *get_error() const & {
    return is_fail() ? &std::get<E>(result) : nullptr;
  }

  constexpr T value_or(const T &default_value) const & {
    return is_success() ? std::get<T>(result) : default_value;
  }

  constexpr T value_or(T &&default_value) && {
    return is_success() ? std::get<T>(std::move(result))
                        : std::forward<T>(default_value);
  }

  //  NEED ENSURE CALLABLE RETURN NON-RESULT
  //  change T in result 
  template <typename callable> auto map(callable &&transform_callable) & {
    //  BUILD RETURN TYPE OF CALLABLE
    using RawR = std::invoke_result_t<callable, T &>;
    // 核心修正：如果 callable 返回 void，映射为 Unit；否则保持原样
    using R = std::conditional_t<std::is_same_v<RawR, void>, Unit,
                                 std::decay_t<RawR>>;

    if (is_success()) {
      if constexpr (std::is_same_v<RawR, void>) {
        // 必须显式调用，然后返回 Unit
        std::forward<callable>(transform_callable)(
            std::get<T>(result));    // 执行函数（副作用）
        return Result<R, E>::success(Unit{}); // 返回包含 Unit 的 Result
      } else {
        return Result<R, E>::success(
            std::forward<callable>(transform_callable)(
                std::get<T>(result))); // 正常执行变换，将结果包装进 Result
      }
    } else {
      return Result<R, E>(ErrorTag{},std::get<E>(result));
    }
  }

  template <typename callable> auto map(callable &&transform_callable) const & {
    //  BUILD RETURN TYPE OF CALLABLE
    using RawR = std::invoke_result_t<callable, const T &>;
    // 核心修正：如果 callable 返回 void，映射为 Unit；否则保持原样
    using R = std::conditional_t<std::is_same_v<RawR, void>, Unit,
                                 std::decay_t<RawR>>;

    if (is_success()) {
      if constexpr (std::is_same_v<RawR, void>) {
        // 必须显式调用，然后返回 Unit
        std::forward<callable>(transform_callable)(
            std::get<T>(result));    // 执行函数（副作用）
        return Result<R, E>::success(Unit{}); // 返回包含 Unit 的 Result
      } else {
        return Result<R, E>::success(
            std::forward<callable>(transform_callable)(
                std::get<T>(result))); // 正常执行变换，将结果包装进 Result
      }
    } else {
      return Result<R, E>(ErrorTag{},std::get<E>(result));
    }
  }

  template <typename callable> auto map(callable &&transform_callable) && {
    //  BUILD RETURN TYPE OF CALLABLE
    using RawR = std::invoke_result_t<callable, T &&>;
    // 核心修正：如果 callable 返回 void，映射为 Unit；否则保持原样
    using R = std::conditional_t<std::is_same_v<RawR, void>, Unit,
                                 std::decay_t<RawR>>;

    if (is_success()) {
      if constexpr (std::is_same_v<RawR, void>) {
        // 必须显式调用，然后返回 Unit
        std::forward<callable>(transform_callable)(
            std::get<T>(std::move(result))); // 执行函数（副作用）
        return Result<R, E>::success(Unit{}); // 返回包含 Unit 的 Result
      } else {
        return Result<R, E>::success(
            std::forward<callable>(transform_callable)(
                std::get<T>(std::move(result)))); // 正常执行变换，将结果包装进 Result
      }
    } else {
      return Result<R, E>(ErrorTag{},std::get<E>(std::move(result)));
    }
  }

  template <typename callable>
  auto map(callable &&transform_callable) const && {
    //  BUILD RETURN TYPE OF CALLABLE
    using RawR = std::invoke_result_t<callable, const T &&>;
    // 核心修正：如果 callable 返回 void，映射为 Unit；否则保持原样
    using R = std::conditional_t<std::is_same_v<RawR, void>, Unit,
                                 std::decay_t<RawR>>;

    if (is_success()) {
      if constexpr (std::is_same_v<RawR, void>) {
        // 必须显式调用，然后返回 Unit
        std::forward<callable>(transform_callable)(
            std::get<T>(std::move(result))); // 执行函数（副作用）
        return Result<R, E>::success(Unit{}); // 返回包含 Unit 的 Result
      } else {
        return Result<R, E>::success(
            std::forward<callable>(transform_callable)(
                std::get<T>(std::move(result)))); // 正常执行变换，将结果包装进 Result
      }
    } else {
      return Result<R, E>(ErrorTag{},std::get<E>(std::move(result)));
    }
  }

  // transformCallable must be not void
  //  change E in result 
  template <typename callable> auto map_err(callable &&transform_callable) & {
    //  BUILD RETURN TYPE OF CALLABLE
    using NewError = std::decay_t<std::invoke_result_t<callable, E &>>;
    static_assert(!std::is_same_v<NewError, void>,
                  "map_err callable must not return void");
    if (is_fail()) {
      return Result<T, NewError>(ErrorTag{},std::forward<callable>(transform_callable)(
          std::get<E>(result))); // 正常执行变换，将结果包装进 Result
    } else {
      return Result<T, NewError>(SuccessTag{},std::get<T>(result));
    }
  }

  template <typename callable>
  auto map_err(callable &&transform_callable) const & {
    //  BUILD RETURN TYPE OF CALLABLE
    using NewError = std::decay_t< std::invoke_result_t<callable, const E &> >;
    static_assert(!std::is_same_v<NewError, void>,
                  "map_err callable must not return void");
    if (is_fail()) {
      return Result<T, NewError>(ErrorTag{},std::forward<callable>(transform_callable)(
          std::get<E>(result))); // 正常执行变换，将结果包装进 Result
    } else {
      return Result<T, NewError>(SuccessTag{},std::get<T>(result));
    }
  }

  template <typename callable> auto map_err(callable &&transform_callable) && {
    //  BUILD RETURN TYPE OF CALLABLE
    using NewError = std::decay_t< std::invoke_result_t<callable, E &&> >;
    static_assert(!std::is_same_v<NewError, void>,
                  "map_err callable must not return void");
    if (is_fail()) {
      return Result<T, NewError>(ErrorTag{},std::forward<callable>(transform_callable)(
          std::get<E>(std::move(result)))); // 正常执行变换，将结果包装进 Result
    } else {
      return Result<T, NewError>(SuccessTag{},std::get<T>(std::move(result)));
    }
  }

  template <typename callable>
  auto map_err(callable &&transform_callable) const && {
    //  BUILD RETURN TYPE OF CALLABLE
    using NewError = std::decay_t< std::invoke_result_t<callable, const E &&> >;
    static_assert(!std::is_same_v<NewError, void>,
                  "map_err callable must not return void");
    if (is_fail()) {
      return Result<T, NewError>(ErrorTag{},std::forward<callable>(transform_callable)(
          std::get<E>(std::move(result)))); // 正常执行变换，将结果包装进 Result
    } else {
      return Result<T, NewError>(SuccessTag{},std::get<T>(std::move(result)));
    }
  }

  //  NEED ENSURE CALLABLE RETURN RESULT
  // transform old result into new other result
  template <typename callable> auto and_then(callable &&transform_callable) & {
    using RawReturnType = std::invoke_result_t<callable, T &>;
    using ReturnType = std::decay_t<RawReturnType>; 

    // 直接使用类型 trait，编译期常量
    static_assert(is_result_type<ReturnType>::value,
                  "and_then's callable must return a Result type");

    // 3. 提取 callable 返回的 Result 的成功和错误类型
    using ReturnErrorType = typename ReturnType::ErrorType;
    static_assert(std::is_same_v<E, ReturnErrorType>,
                  "and_then: error type must match");

    if (is_success()) {
      // 成功路径：直接返回 callable 的结果
      return std::forward<callable>(transform_callable)(std::get<T>(result));
    } else {
      // 错误路径：构造失败状态的 ReturnType
      // 关键：用当前错误值构造 ReturnType，而不是直接返回错误值
      return ReturnType(ErrorTag{},std::get<E>(result));
    }
  }

  template <typename callable>
  auto and_then(callable &&transform_callable) const & {
    using RawReturnType = std::invoke_result_t<callable, const T &>;
    using ReturnType = std::decay_t<RawReturnType>; 

    // 直接使用类型 trait，编译期常量
    static_assert(is_result_type<ReturnType>::value,
                  "and_then's callable must return a Result type");

    // 3. 提取 callable 返回的 Result 的成功和错误类型
    using ReturnErrorType = typename ReturnType::ErrorType;
    static_assert(std::is_same_v<E, ReturnErrorType>,
              "and_then: error type must match");

    if (is_success()) {
      // 成功路径：直接返回 callable 的结果
      return std::forward<callable>(transform_callable)(std::get<T>(result));
    } else {
      // 错误路径：构造失败状态的 ReturnType
      // 关键：用当前错误值构造 ReturnType，而不是直接返回错误值
      return ReturnType(ErrorTag{},std::get<E>(result));
    }
  }

  template <typename callable> auto and_then(callable &&transform_callable) && {
    using RawReturnType = std::invoke_result_t<callable, T &&>;
    using ReturnType = std::decay_t<RawReturnType>; 

    // 直接使用类型 trait，编译期常量
    static_assert(is_result_type<ReturnType>::value,
                  "and_then's callable must return a Result type");

    // 3. 提取 callable 返回的 Result 的成功和错误类型
    using ReturnErrorType = typename ReturnType::ErrorType;
    static_assert(std::is_same_v<E, ReturnErrorType>,
              "and_then: error type must match");

    if (is_success()) {
      // 成功路径：直接返回 callable 的结果
      return std::forward<callable>(transform_callable)(
          std::get<T>(std::move(result)));
    } else {
      // 错误路径：构造失败状态的 ReturnType
      // 关键：用当前错误值构造 ReturnType，而不是直接返回错误值
      return ReturnType(ErrorTag{},std::get<E>(std::move(result)));
    }
  }

  template <typename callable>
  auto and_then(callable &&transform_callable) const && {
    using RawReturnType = std::invoke_result_t<callable, const T &&>;
    using ReturnType = std::decay_t<RawReturnType>; 

    // 直接使用类型 trait，编译期常量
    static_assert(is_result_type<ReturnType>::value,
                  "and_then's callable must return a Result type");

    // 3. 提取 callable 返回的 Result 的成功和错误类型
    using ReturnErrorType = typename ReturnType::ErrorType;
    static_assert(std::is_same_v<E, ReturnErrorType>,
              "and_then: error type must match");

    if (is_success()) {
      // 成功路径：直接返回 callable 的结果
      return std::forward<callable>(transform_callable)(
          std::get<T>(std::move(result)));
    } else {
      // 错误路径：构造失败状态的 ReturnType
      // 关键：用当前错误值构造 ReturnType，而不是直接返回错误值
      return ReturnType(ErrorTag{},std::get<E>(std::move(result)));
    }
  }

  //  NEED ENSURE CALLABLE RETURN RESULT
  // transform old result into new other result
  template <typename callable> auto or_else(callable &&recover_callable) & {
    using RawReturnType = std::invoke_result_t<callable, E &>;
    using ReturnType = std::decay_t<RawReturnType>; 

    using ReturnValueType = typename ReturnType::ValueType;
    // 直接使用类型 trait，编译期常量
    static_assert(is_result_type<ReturnType>::value,
                  "and_then's callable must return a Result type");

    static_assert(std::is_same_v<T, ReturnValueType>,
                  "or_else: success type must match");

    if (is_fail()) {
      return std::forward<callable>(recover_callable)(std::get<E>(result));
    } else {
      return ReturnType{SuccessTag{},std::get<T>(result)};
    }
  }

  template <typename callable>
  auto or_else(callable &&recover_callable) const & {
    using RawReturnType = std::invoke_result_t<callable, const E &>;
    using ReturnType = std::decay_t<RawReturnType>; 

    using ReturnValueType = typename ReturnType::ValueType;
    // 直接使用类型 trait，编译期常量
    static_assert(is_result_type<ReturnType>::value,
                  "and_then's callable must return a Result type");
    static_assert(std::is_same_v<T, ReturnValueType>,
                  "or_else: success type must match");

    if (is_fail()) {
      return std::forward<callable>(recover_callable)(std::get<E>(result));
    } else {
      return ReturnType{SuccessTag{},std::get<T>(result)};
    }
  }

  template <typename callable> auto or_else(callable &&recover_callable) && {
    using RawReturnType = std::invoke_result_t<callable, E &&>;
    using ReturnType = std::decay_t<RawReturnType>; 

     using ReturnValueType = typename ReturnType::ValueType;
    // 直接使用类型 trait，编译期常量
    static_assert(is_result_type<ReturnType>::value,
                  "and_then's callable must return a Result type");
    static_assert(std::is_same_v<T, ReturnValueType>,
                  "or_else: success type must match");

    if (is_fail()) {
      return std::forward<callable>(recover_callable)(
          std::get<E>(std::move(result)));
    } else {
      return ReturnType{SuccessTag{},std::get<T>(std::move(result))};
    }
  }

  template <typename callable>
  auto or_else(callable &&recover_callable) const && {
    using RawReturnType = std::invoke_result_t<callable, const E &&>;
    using ReturnType = std::decay_t<RawReturnType>; 

      using ReturnValueType = typename ReturnType::ValueType;
    // 直接使用类型 trait，编译期常量
    static_assert(is_result_type<ReturnType>::value,
                  "and_then's callable must return a Result type");
    static_assert(std::is_same_v<T, ReturnValueType>,
                  "or_else: success type must match");

    if (is_fail()) {
      return std::forward<callable>(recover_callable)(
          std::get<E>(std::move(result)));
    } else {
      return ReturnType{SuccessTag{},std::get<T>(std::move(result))};
    }
  }

  // 提取值并提供默认值（Rust 语义：成功返回 f(value)，失败返回 default）。
  // 返回的是普通值，不是 Result；因此不能构造 Result 的 tag 对象。
  template <typename F, typename U>
  auto map_or(F &&f, U &&default_value) & {
    using ReturnType =
        std::common_type_t<std::invoke_result_t<F, T &>, std::decay_t<U>>;
    if (is_success())
      return ReturnType(std::forward<F>(f)(std::get<T>(result)));
    return ReturnType(std::forward<U>(default_value));
  }

  template <typename F, typename U>
  auto map_or(F &&f, U &&default_value) const & {
    using ReturnType =
        std::common_type_t<std::invoke_result_t<F, const T &>, std::decay_t<U>>;
    if (is_success())
      return ReturnType(std::forward<F>(f)(std::get<T>(result)));
    return ReturnType(std::forward<U>(default_value));
  }

  template <typename F, typename U>
  auto map_or(F &&f, U &&default_value) && {
    using ReturnType =
        std::common_type_t<std::invoke_result_t<F, T &&>, std::decay_t<U>>;
    if (is_success())
      return ReturnType(
          std::forward<F>(f)(std::get<T>(std::move(result))));
    return ReturnType(std::forward<U>(default_value));
  }

  template <typename F, typename U>
  auto map_or(F &&f, U &&default_value) const && {
    using ReturnType =
        std::common_type_t<std::invoke_result_t<F, const T &&>, std::decay_t<U>>;
    if (is_success())
      return ReturnType(
          std::forward<F>(f)(std::get<T>(std::move(result))));
    return ReturnType(std::forward<U>(default_value));
  }

  // 提取值并提供函数计算的默认值（Rust 语义，返回普通值）。
  template <typename F, typename U> auto map_or_else(F &&f, U &&default_value) & {
    using ReturnType =
        std::common_type_t<std::invoke_result_t<F, T &>, std::invoke_result_t<U>>;
    if (is_success())
      return ReturnType(std::forward<F>(f)(std::get<T>(result)));
    return ReturnType(std::forward<U>(default_value)());
  }

  template <typename F, typename U> auto map_or_else(F &&f, U &&default_value) const & {
    using ReturnType =
        std::common_type_t<std::invoke_result_t<F,const T &>, std::invoke_result_t<U>>;
    if (is_success())
      return ReturnType(std::forward<F>(f)(std::get<T>(result)));
    return ReturnType(std::forward<U>(default_value)());
  }

  template <typename F, typename U> auto map_or_else(F &&f, U &&default_value)  && {
    using ReturnType =
        std::common_type_t<std::invoke_result_t<F, T &&>, std::invoke_result_t<U>>;
    if (is_success())
      return ReturnType(
          std::forward<F>(f)(std::get<T>(std::move(result))));
    return ReturnType(std::forward<U>(default_value)());
  }

  template <typename F, typename U>
  auto map_or_else(F &&f, U &&default_value) const && {
    using ReturnType = std::common_type_t<std::invoke_result_t<F, const T &&>,
                                          std::invoke_result_t<U>>;
    if (is_success())
      return ReturnType(
          std::forward<F>(f)(std::get<T>(std::move(result))));
    return ReturnType(std::forward<U>(default_value)());
  }
};
