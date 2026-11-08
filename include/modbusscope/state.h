#pragma once

namespace modbusscope {

enum class ConnState : int {
    Idle,       ///< 空闲，允许发起连接操作
    Connecting  ///< 正在执行连接操作（已持有互斥锁）
};


// 会话状态机公开枚举（供 state() 查询与测试断言）。
enum class LifeState : int {
  RUNNING,

  RECOVERING,

  GIVEN_UP,
};

// 状态机事件。公开仅为测试/可观测性；不提供外部触发事件的公共 API。
enum class SessionEvent : int {
  Connect,
  ConnectSucceeded,
  ConnectFailed,
  RequestIssued,
  RequestSucceeded,
  RequestFailed,
  Disconnect,
  Shutdown,
  NullPtr,
  ThreadBusy,
  Unknown 
};

const char* to_string(SessionEvent event) noexcept;

} // namespace modbusscope