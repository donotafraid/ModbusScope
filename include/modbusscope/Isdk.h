#pragma once

#include <QModbusTcpClient>
#include <QModbusDataUnit>
#include <QModbusRequest>
#include <QModbusResponse>
#include <QModbusReply>


#include "modbusscope/config.h"    // ClientConfig / TcpEndpoint
#include "modbusscope/error.h"     // ErrorCode
#include "modbusscope/request.h"   // ReadRequest / FunctionCode

namespace modbusscope {

// Modbus 客户端 SDK 抽象。实现分两类：
//   - QtModbusSdk：生产实现，封装 Qt5::SerialBus；内部用 QEventLoop 把
//     QModbusReply 的异步完成适配为同步返回。
//   - MockSdk：测试实现，不依赖事件循环，按脚本返回结果或注入故障。
//
// 线程安全：ISdk 实现本身不保证线程安全；线程安全由 Client::Impl 的
// m_transportMutex 串行化。ISdk 方法可在任意线程调用，但同一 handle 的
// 调用必须由调用方串行化（见锁表 m_transportMutex）。
class ISdk {
public:
    virtual ~ISdk() = default;

    // ---- 生命周期 ----
    // 创建客户端 handle。config 为值传入，实现内部拷贝所需字段。
    // 返回 nullptr 表示创建失败（分配失败或配置非法）。
    // 所有权：handle 归 ISdk 实现所有，调用方不得 delete。
    virtual QModbusClient* clientNew(const ClientConfig& config) = 0;

    // 销毁 handle。实现必须在销毁前 abort 所有 pending reply。
    // 前置条件：该 handle 上无在途请求（由 Client::Impl 的 InFlightGuard 保证）。
    virtual void clientDelete(QModbusClient* client) noexcept = 0;

    // ---- 连接 ----
    // 发起异步连接；返回 Ok 表示已发起，不代表已连上。
    // 连接结果通过 getState 轮询或 stateChanged 信号观测。
    // 失败：InvalidArgument（host 空/端口 0）、ConnectionFailed（connectDevice 返回 false）。
    virtual bool connectAsync(QModbusClient* c,
                                   const TcpEndpoint& endpoint) = 0;

    // 发起异步断开；返回 Ok 表示已发起。
    virtual ErrorCode disconnectAsync(QModbusClient* c) = 0;

    // 同步断开：阻塞直到 UnconnectedState 或 config.connectTimeoutMs 超时。
    // 超时返回 Timeout；已断开返回 Ok。
    virtual ErrorCode disconnect(QModbusClient* c) = 0;

    // ---- 服务 ----
    // 同步读：内部 sendReadRequest + 等待 reply.finished（受 requestTimeoutMs 约束）。
    // 成功：返回 Ok，*response 被填充为 QModbusDataUnit
    //       （registerType 对应 0x03/0x04，startAddress/valueCount 回填，
    //        value() 为寄存器值）。
    // 失败：返回非 Ok；*response 保持调用前状态（实现不得写入部分结果）。
    // 所有权：QModbusDataUnit 是值类型，随输出参数转移/拷贝，无堆指针泄漏风险。
    // 错误映射：
    //   QModbusDevice::TimeoutError        -> Timeout
    //   QModbusDevice::ProtocolError       -> ProtocolError
    //   QModbusDevice::ConnectionError     -> ConnectionFailed
    //   QModbusDevice::ReplyAbortedError   -> Closed
    //   QModbusExceptionResponse           -> ProtocolError
    //   quantity 越界 / 功能码非 0x03/0x04 -> InvalidArgument / UnsupportedFunction
    virtual QModbusReply* serviceRead(QModbusClient* c,
                                  const ReadRequest& req,
                                  QModbusDataUnit* response) = 0;

    // 同步写（0x06/0x10；本 SDK 仅用于内部测试路径，公共 API 不暴露写）。
    // 成功返回 Ok；失败返回映射后的 ErrorCode。
    virtual QModbusReply* serviceWrite(QModbusClient* c,
                                   const WriteRequest& req,
                                   const QModbusDataUnit& values) = 0;

    // 单调时钟毫秒。生产实现用 QDateTime::currentMSecsSinceEpoch()；
    // 测试实现用 FakeClock。
    virtual std::int64_t nowMs() = 0;
};

} // namespace modbusscope