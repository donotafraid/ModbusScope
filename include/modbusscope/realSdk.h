// sdk/RealSdk.h
#pragma once
#include "include/modbusscope/Isdk.h"   // 按工程实际路径
#include "modbusscope/request.h"
#include <QEventLoop>
#include <QModbusClient>
#include <QModbusDataUnit>
#include <QModbusDevice>
#include <QModbusReply>
#include <QModbusTcpClient>
#include <QTimer>
#include <QVector>
#include <QVariant>

#include <chrono>
#include <cstdint>
#include <new>
#include <qmodbusdataunit.h>
#include <string>

namespace modbusscope {

// RealSdk：ISdk 的生产实现，封装 Qt5::SerialBus。
// 线程安全：不保证。同一 handle 的调用由 Client::Impl 的 m_transportMutex 串行化。
// 公共头零 Qt；本文件仅内部使用。
class RealSdk final : public ISdk {
public:
    RealSdk() = default;

    ~RealSdk() = default;

    // ---- 生命周期 ----
    // 仅负责分配并应用 ClientConfig；host 为空不在 clientNew 拒绝，
    // 由 connectAsync 返回 InvalidArgument。
    QModbusClient* clientNew(const ClientConfig& config) override {
        auto* client = new (std::nothrow) QModbusTcpClient();
        if (!client) return nullptr;

        client->setConnectionParameter(
            QModbusDevice::NetworkAddressParameter,
            QString::fromStdString(config.endpoint.host));
        client->setConnectionParameter(
            QModbusDevice::NetworkPortParameter,
            static_cast<int>(config.endpoint.port));
        client->setTimeout(static_cast<int>(config.requestTimeoutMs));
        client->setNumberOfRetries(0);   // 重试由 Recovering 状态机驱动

        return client;
    }

    void clientDelete(QModbusClient* client) noexcept override {
        if (!client) return;
        delete client;
    }

    // ---- 连接 ----
    bool connectAsync(QModbusClient* c, const TcpEndpoint& endpoint) override {
        // 运行期不允许热更新；此处的 endpoint 覆盖仅用于构造后首次连接。
        c->setConnectionParameter(
            QModbusDevice::NetworkAddressParameter,
            QString::fromStdString(endpoint.host));
        c->setConnectionParameter(
            QModbusDevice::NetworkPortParameter,
            static_cast<int>(endpoint.port));

        return c->connectDevice();
    }

    ErrorCode disconnectAsync(QModbusClient* c) override {
        if (!c) return ErrorCode::InvalidArgument;
        c->disconnectDevice();
        return ErrorCode::Ok;
    }

    ErrorCode disconnect(QModbusClient* c) override {
        if (!c) return ErrorCode::InvalidArgument;
        if (c->state() == QModbusDevice::UnconnectedState) return ErrorCode::Ok;

        c->disconnectDevice();
        return (c->state() == QModbusDevice::UnconnectedState)
                   ? ErrorCode::Ok
                   : ErrorCode::Timeout;
    }

    // ---- 服务 ----
    // 前置条件：c 非空、response 非空、quantity ∈ [1,125]、function ∈ {0x03,0x04}。
    // 成功：Ok 且 *response 填好；失败：非 Ok 且 *response 保持调用前状态。
    QModbusReply * serviceRead(QModbusClient* c, const ReadRequest& req,
                          QModbusDataUnit* response) override {
        return c->sendReadRequest(*response, req.unitId);
    }

    // 内部测试路径；公共 API 不暴露写。
    QModbusReply *serviceWrite(QModbusClient *c, const WriteRequest &req,
                               const QModbusDataUnit &values) override {
      QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, req.startAddress,
                           values.valueCount());
      unit.setValues(values.values());

      return c->sendWriteRequest(unit, req.unitId);
    }

    std::int64_t nowMs() override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

private:
    // // 阻塞等待 reply.finished，超时 abort 并返回 Timeout。
    // // 调用方负责 reply->deleteLater()。
    // static ErrorCode waitForReply(QModbusReply* reply, std::uint32_t timeoutMs) {
    //     if (reply->isFinished()) return ErrorCode::Ok;

    //     QEventLoop loop;
    //     QTimer timer;
    //     timer.setSingleShot(true);
    //     QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    //     QObject::connect(reply, &QModbusReply::finished,
    //                      &loop, &QEventLoop::quit);
    //     timer.start(static_cast<int>(timeoutMs));
    //     loop.exec();

    //     if (!reply->isFinished()) {
    //         return ErrorCode::Timeout;
    //     }
    //     return ErrorCode::Ok;
    // }

    // static SessionState mapState(QModbusDevice::State s) noexcept {
    //     switch (s) {
    //         case QModbusDevice::UnconnectedState: return SessionState::Idle;
    //         case QModbusDevice::ConnectingState:  return SessionState::Connecting;
    //         case QModbusDevice::ConnectedState:   return SessionState::Ready;
    //         case QModbusDevice::ClosingState:     return SessionState::Recovering;
    //     }
    //     return SessionState::Closed;
    // }

    // static ErrorCode mapDeviceError(QModbusDevice::Error e) noexcept {
    //     switch (e) {
    //         case QModbusDevice::NoError:            return ErrorCode::Ok;
    //         case QModbusDevice::ReadError:          return ErrorCode::ProtocolError;
    //         case QModbusDevice::WriteError:         return ErrorCode::ProtocolError;
    //         case QModbusDevice::ConnectionError:    return ErrorCode::ConnectionFailed;
    //         case QModbusDevice::ConfigurationError: return ErrorCode::InvalidArgument;
    //         case QModbusDevice::TimeoutError:       return ErrorCode::Timeout;
    //         case QModbusDevice::ProtocolError:      return ErrorCode::ProtocolError;
    //         case QModbusDevice::ReplyAbortedError:  return ErrorCode::Closed;
    //         case QModbusDevice::UnknownError:       return ErrorCode::Internal;
    //     }
    //     return ErrorCode::Internal;
    // }

};

} // namespace modbusscope