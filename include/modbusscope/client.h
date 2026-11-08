#pragma once
#include <memory>
#include "modbusscope/config.h"
#include "modbusscope/recreatSync.h"
#include "modbusscope/request.h"
#include "modbusscope/state.h"
#include "Rust_error_deal/error_deal.h"

namespace modbusscope {

// 多线程共享实例；内部同步；公共头不出现 Qt 类型。
// 线程安全等级：内部同步的共享实例。
//   - read()/connect()/disconnect()/state()/shutdown() 可并发调用；
//   - 无配置写入入口，因此无“单写者”约束。
// 调用方责任：
//   - 销毁必须在所有线程停止调用后进行；
//   - 不得在回调中同步调用会阻塞等待自身的操作（见不变式 I4）。
class Client {
public:
    // 唯一创建入口。失败返回 Error，不抛异常（含分配失败 -> Internal）。
    static Result<std::unique_ptr<Client>,RichError> create(ClientConfig config) noexcept;

    ~Client() noexcept;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    // 建立连接。幂等：
    //   Ready/Connecting -> Ok；Closed -> Error(Closed)；
    //   Idle/Recovering -> 发起连接，失败返回 ConnectionFailed/Timeout。
    // 不抛异常；返回时状态已提交。
    Result<Unit,RichError> connect() noexcept;

    // 断开连接。幂等：
    //   Idle -> Ok；Closed -> Error(Closed)；
    //   其他 -> 关闭 transport，迁移到 Idle。
    Result<Unit,RichError> disconnect() noexcept;

    // 发起 0x03/0x04 读取；多线程并发安全。
    // 返回：
    //   成功 -> ReadResponse；
    //   状态非 Ready -> NotConnected；Closed -> Closed；
    //   超时 -> Timeout；协议错误 -> ProtocolError；
    //   非法 quantity/功能码 -> InvalidArgument/UnsupportedFunction。
    // 契约：返回前在途计数归零；不抛异常。
    Result<ReadResponse,RichError> read(const ReadRequest& request) noexcept;

    // 查询状态快照。不阻塞；Closed 后恒为 Closed。
    Result<LifeState,RichError> state() const noexcept;

    // 关闭：等待在途请求归零（带超时），关闭 transport，进入 Closed 终态。
    // 幂等：Closed 上调用返回 Ok。不抛异常。
    Result<Unit,RichError> shutdown() noexcept;

private:
    class Impl;
    // Impl需要的回调上下文
    struct CallbackContext;
    // 使用 RAII 包装器
    struct QModbus_ClientDeleter;
    explicit Client(ClientConfig config) ;
    std::shared_ptr<Impl> m_impl;
    std::shared_ptr<RecreateSync> m_recreateSync; // 与 RecreateGuard 共享
};

} // namespace modbusscope