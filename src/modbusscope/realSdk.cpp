#include "detail/realSdk.h"
#include "detail/threadName.h"

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QModbusDataUnit>
#include <QModbusDevice>
#include <QModbusPdu>
#include <QModbusReply>
#include <QModbusRequest>
#include <QModbusTcpClient>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace modbusscope::detail {
namespace {

// Snapshot used by state(); it has no protocol timeout of its own.
constexpr std::uint32_t kStateWaitMs = 2000;

ErrorCode map_device_error(QModbusDevice::Error error) noexcept {
    switch (error) {
    case QModbusDevice::NoError:            return ErrorCode::Ok;
    case QModbusDevice::ReadError:          return ErrorCode::ProtocolError;
    case QModbusDevice::WriteError:         return ErrorCode::ProtocolError;
    case QModbusDevice::ConnectionError:    return ErrorCode::ConnectionFailed;
    case QModbusDevice::ConfigurationError: return ErrorCode::InvalidArgument;
    case QModbusDevice::TimeoutError:       return ErrorCode::Timeout;
    case QModbusDevice::ProtocolError:      return ErrorCode::ProtocolError;
    case QModbusDevice::ReplyAbortedError:  return ErrorCode::NotConnected;
    case QModbusDevice::UnknownError:       return ErrorCode::Internal;
    }
    return ErrorCode::Internal;
}

// Same bound as Client::Impl::kMaxOperationTimeoutMs. Keeping it local avoids
// installing a detail header just for one constant.
constexpr std::uint64_t kMaxTimeoutMs = 600000;

std::uint64_t caller_wait_ms(std::uint32_t timeoutMs) noexcept {
    // Clamp before adding the grace. A raw uint32_t + 2000 wraps for a
    // configured value near UINT32_MAX and would turn a multi-minute budget
    // into a ~2 s false timeout.
    return std::min<std::uint64_t>(timeoutMs, kMaxTimeoutMs) +
           kSdkCallWaitGraceMs;
}

int bounded_timeout(std::uint32_t timeoutMs) noexcept {
    const auto capped = std::min<std::uint64_t>(timeoutMs, kMaxTimeoutMs);
    return static_cast<int>(std::max<std::uint64_t>(capped, 10));
}

void append_device_error(std::string *detail, const QModbusDevice *device) {
    if (!detail || !device) {
        return;
    }
    const QString text = device->errorString();
    if (!text.isEmpty()) {
        if (!detail->empty()) {
            detail->append(": ");
        }
        detail->append(text.toStdString());
    }
}

void append_exception_code(std::string *detail, const QModbusResponse &raw) {
    if (!detail || !raw.isException()) {
        return;
    }
    if (!detail->empty()) {
        detail->append(": ");
    }
    detail->append("exception 0x");
    detail->append(std::to_string(
        static_cast<unsigned>(raw.exceptionCode())));
}

// Every QModbusReply exits through this guard, including the exception path.
// The reply's parent is the QModbusClient, so deleteLater() is the documented
// release mechanism and it runs on the transport thread.
class ReplyGuard {
public:
    explicit ReplyGuard(QModbusReply *reply) : m_reply(reply) {}
    ~ReplyGuard() {
        if (m_reply) {
            m_reply->deleteLater();
        }
    }

    ReplyGuard(const ReplyGuard &) = delete;
    ReplyGuard &operator=(const ReplyGuard &) = delete;

private:
    QModbusReply *m_reply = nullptr;
};

struct ConnectCall {
    TcpEndpoint endpoint;
    std::uint32_t timeoutMs = 0;
    std::string detail;
    ErrorCode code = ErrorCode::Internal;
    std::exception_ptr exception;
};

struct DisconnectCall {
    std::uint32_t timeoutMs = 0;
    std::string detail;
    ErrorCode code = ErrorCode::Internal;
    std::exception_ptr exception;
};

struct ReadCall {
    ReadRequest request;
    std::uint32_t timeoutMs = 0;
    ReadResponse response;
    std::string detail;
    ErrorCode code = ErrorCode::Internal;
    std::exception_ptr exception;
};

struct WriteCall {
    WriteRequest request;
    std::uint32_t timeoutMs = 0;
    WriteResponse response;
    std::string detail;
    ErrorCode code = ErrorCode::Internal;
    std::exception_ptr exception;
};

struct StateCall {
    LifeState value = LifeState::IDLE;
    std::exception_ptr exception;
};

class Transport final : public QObject {
public:
    explicit Transport(std::uint32_t requestTimeoutMs)
        : m_requestTimeoutMs(requestTimeoutMs) {
        m_client = std::make_unique<QModbusTcpClient>();
        m_client->setTimeout(bounded_timeout(m_requestTimeoutMs));
        // The wrapper owns retries; Qt's hidden retries would make the public
        // requestTimeoutMs budget impossible to reason about.
        m_client->setNumberOfRetries(0);
    }

    ~Transport() override {
        if (m_client) {
            m_client->disconnectDevice();
            m_client.reset();
        }
    }

    ErrorCode connectSync(const TcpEndpoint &endpoint, std::uint32_t timeoutMs,
                          std::string *detail) {
        if (!m_client) {
            return ErrorCode::Internal;
        }
        if (m_client->state() == QModbusDevice::ConnectedState) {
            return ErrorCode::Ok;
        }
        if (m_client->state() != QModbusDevice::UnconnectedState) {
            resetClient();
        }

        m_client->setConnectionParameter(
            QModbusDevice::NetworkAddressParameter,
            QString::fromStdString(endpoint.host));
        m_client->setConnectionParameter(
            QModbusDevice::NetworkPortParameter,
            static_cast<int>(endpoint.port));
        // The QEventLoop below is the connect budget. Do not put the connect
        // timeout into QModbusClient::setTimeout: that property is the response
        // budget for every later read/write.
        m_client->setNumberOfRetries(0);

        if (!m_client->connectDevice()) {
            const ErrorCode code = map_device_error(m_client->error());
            append_device_error(detail, m_client.get());
            resetClient();
            return code == ErrorCode::Ok ? ErrorCode::ConnectionFailed : code;
        }

        if (m_client->state() == QModbusDevice::ConnectedState) {
            configure_connected_socket();
            return ErrorCode::Ok;
        }

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(m_client.get(), &QModbusDevice::stateChanged, &loop,
                         [&loop](QModbusDevice::State state) {
                             if (state == QModbusDevice::ConnectedState ||
                                 state == QModbusDevice::UnconnectedState) {
                                 loop.quit();
                             }
                         });
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(bounded_timeout(timeoutMs));
        loop.exec();

        if (m_client->state() == QModbusDevice::ConnectedState) {
            configure_connected_socket();
            return ErrorCode::Ok;
        }

        const ErrorCode code = map_device_error(m_client->error());
        append_device_error(detail, m_client.get());
        // A half-open socket must not be reused. Recreate it so the next
        // connect starts from UnconnectedState.
        resetClient();
        if (code != ErrorCode::Ok) {
            return code;
        }
        if (detail && detail->empty()) {
            *detail = "connect timed out";
        }
        return ErrorCode::Timeout;
    }

    ErrorCode disconnectSync(std::uint32_t timeoutMs, std::string *detail) {
        if (!m_client) {
            return ErrorCode::Ok;
        }
        if (m_client->state() == QModbusDevice::UnconnectedState) {
            return ErrorCode::Ok;
        }

        m_client->disconnectDevice();
        if (m_client->state() == QModbusDevice::UnconnectedState) {
            return ErrorCode::Ok;
        }

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(m_client.get(), &QModbusDevice::stateChanged, &loop,
                         [&loop](QModbusDevice::State state) {
                             if (state == QModbusDevice::UnconnectedState) {
                                 loop.quit();
                             }
                         });
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(bounded_timeout(timeoutMs));
        loop.exec();

        if (m_client->state() == QModbusDevice::UnconnectedState) {
            return ErrorCode::Ok;
        }
        if (detail) {
            *detail = "disconnect timed out";
        }
        resetClient();
        return ErrorCode::Timeout;
    }

    ErrorCode readSync(const ReadRequest &request, std::uint32_t timeoutMs,
                       ReadResponse *response, std::string *detail) {
        if (!m_client || !response) {
            return ErrorCode::Internal;
        }
        if (request.quantity == 0) {
            return ErrorCode::InvalidArgument;
        }
        if (m_client->state() != QModbusDevice::ConnectedState) {
            return ErrorCode::NotConnected;
        }

        // Restore the response budget on every request. connectSync() no longer
        // overwrites it with the connect budget.
        m_client->setTimeout(bounded_timeout(timeoutMs));

        const QModbusDataUnit::RegisterType type =
            request.function == FunctionCode::ReadInputRegisters
                ? QModbusDataUnit::InputRegisters
                : QModbusDataUnit::HoldingRegisters;
        const QModbusDataUnit unit(type, request.startAddress,
                                   static_cast<quint16>(request.quantity));

        QModbusReply *reply = m_client->sendReadRequest(unit, request.unitId);
        if (!reply) {
            const ErrorCode code = map_device_error(m_client->error());
            append_device_error(detail, m_client.get());
            return code == ErrorCode::Ok ? ErrorCode::ProtocolError : code;
        }
        ReplyGuard guard(reply);

        wait_for_reply(reply, timeoutMs);

        if (!reply->isFinished()) {
            // The Qt timeout should have fired first. Destroying a pending reply
            // removes it from QModbusTcpClientPrivate's transaction store.
            if (detail) {
                *detail = "reply did not finish";
            }
            return ErrorCode::Timeout;
        }

        const QModbusDevice::Error error = reply->error();
        const QString errorText = reply->errorString();

        if (error != QModbusDevice::NoError) {
            if (detail && !errorText.isEmpty()) {
                *detail = errorText.toStdString();
            }
            const QModbusResponse raw = reply->rawResult();
            if (error == QModbusDevice::ProtocolError) {
                append_exception_code(detail, raw);
            }
            return map_device_error(error);
        }

        const QModbusDataUnit result = reply->result();
        if (result.valueCount() != request.quantity ||
            result.values().size() != static_cast<int>(request.quantity)) {
            if (detail) {
                *detail = "read response length mismatch";
            }
            return ErrorCode::ProtocolError;
        }

        response->function = request.function;
        response->registers.clear();
        response->registers.reserve(request.quantity);
        for (quint16 value : result.values()) {
            response->registers.push_back(static_cast<std::uint16_t>(value));
        }
        return ErrorCode::Ok;
    }

    ErrorCode writeSync(const WriteRequest &request, std::uint32_t timeoutMs,
                        WriteResponse *response, std::string *detail) {
        if (!m_client || !response) {
            return ErrorCode::Internal;
        }
        if (request.values.size() != request.quantity ||
            request.values.empty()) {
            return ErrorCode::InvalidArgument;
        }
        if (m_client->state() != QModbusDevice::ConnectedState) {
            return ErrorCode::NotConnected;
        }

        m_client->setTimeout(bounded_timeout(timeoutMs));

        QModbusReply *reply = nullptr;
        if (request.function == FunctionCode::WriteSingleRegister) {
            QModbusRequest pdu(QModbusRequest::WriteSingleRegister,
                               static_cast<quint16>(request.startAddress),
                               static_cast<quint16>(request.values.front()));
            reply = m_client->sendRawRequest(pdu, request.unitId);
        } else {
            QVector<quint16> values;
            values.reserve(static_cast<int>(request.values.size()));
            for (std::uint16_t value : request.values) {
                values.push_back(static_cast<quint16>(value));
            }
            const auto byteCount =
                static_cast<quint8>(request.values.size() * sizeof(quint16));
            QModbusRequest pdu(QModbusRequest::WriteMultipleRegisters,
                               static_cast<quint16>(request.startAddress),
                               static_cast<quint16>(request.quantity),
                               byteCount, values);
            reply = m_client->sendRawRequest(pdu, request.unitId);
        }

        if (!reply) {
            const ErrorCode code = map_device_error(m_client->error());
            append_device_error(detail, m_client.get());
            return code == ErrorCode::Ok ? ErrorCode::ProtocolError : code;
        }
        ReplyGuard guard(reply);

        wait_for_reply(reply, timeoutMs);

        if (!reply->isFinished()) {
            if (detail) {
                *detail = "reply did not finish";
            }
            return ErrorCode::Timeout;
        }

        const QModbusDevice::Error error = reply->error();
        const QString errorText = reply->errorString();
        const QModbusResponse raw = reply->rawResult();

        if (error != QModbusDevice::NoError) {
            if (detail && !errorText.isEmpty()) {
                *detail = errorText.toStdString();
            }
            if (error == QModbusDevice::ProtocolError) {
                append_exception_code(detail, raw);
            }
            return map_device_error(error);
        }

        if (!raw.isValid()) {
            if (detail) {
                *detail = "write response is not a valid Modbus PDU";
            }
            return ErrorCode::ProtocolError;
        }
        if (raw.functionCode() != static_cast<QModbusPdu::FunctionCode>(
                                      request.function)) {
            if (detail) {
                *detail = "write response function mismatch";
            }
            return ErrorCode::ProtocolError;
        }

        // 0x06 echoes (address, value); 0x10 echoes (address, quantity).
        // Comparing only the function code accepts a server that echoes the
        // wrong address or count, which is exactly the failure this check is
        // meant to catch.
        if (raw.dataSize() != 4) {
            if (detail) {
                *detail = "write response echo has the wrong length";
            }
            return ErrorCode::ProtocolError;
        }
        quint16 echoAddress = 0;
        quint16 echoSecond = 0;
        raw.decodeData(&echoAddress, &echoSecond);
        if (echoAddress != request.startAddress) {
            if (detail) {
                *detail = "write response address mismatch";
            }
            return ErrorCode::ProtocolError;
        }
        const quint16 expectedSecond =
            request.function == FunctionCode::WriteSingleRegister
                ? static_cast<quint16>(request.values.front())
                : static_cast<quint16>(request.quantity);
        if (echoSecond != expectedSecond) {
            if (detail) {
                *detail = request.function == FunctionCode::WriteSingleRegister
                              ? "write response value mismatch"
                              : "write response quantity mismatch";
            }
            return ErrorCode::ProtocolError;
        }

        response->function = request.function;
        response->startAddress = request.startAddress;
        response->quantity = request.quantity;
        return ErrorCode::Ok;
    }

    QModbusDevice::State state() const noexcept {
        return m_client ? m_client->state() : QModbusDevice::UnconnectedState;
    }

    void shutdown() noexcept {
        if (!m_client) {
            return;
        }
        m_client->disconnectDevice();
        m_client.reset();
    }

private:
    static void wait_for_reply(QModbusReply *reply, std::uint32_t timeoutMs) {
        if (reply->isFinished()) {
            return;
        }
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(reply, &QModbusReply::finished, &loop,
                         &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        // QModbusClient has its own response timeout. This timer is a safety
        // net; it must be strictly later so Qt's TimeoutError wins. Keep the
        // slack small: a +500 ms net made the observed response budget look
        // larger than requestTimeoutMs.
        const auto graceMs =
            std::min<std::uint64_t>(static_cast<std::uint64_t>(timeoutMs) + 200,
                                    600000);
        timer.start(static_cast<int>(graceMs));
        loop.exec();
    }

    void configure_connected_socket() {
        if (!m_client) {
            return;
        }
        // Restore the response budget after a successful connect.
        m_client->setTimeout(bounded_timeout(m_requestTimeoutMs));
        // SO_KEEPALIVE is a last-resort half-open detector. The primary
        // mechanism is the consecutive-timeout escalation in Client::Impl.
        if (auto *socket =
                qobject_cast<QAbstractSocket *>(m_client->device())) {
            socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        }
    }

    void resetClient() {
        auto old = std::move(m_client);
        if (old) {
            old->disconnectDevice();
        }
        // old is destroyed here, before the replacement is allocated. If the
        // allocation throws, m_client stays null instead of dangling.
        old.reset();
        m_client = std::make_unique<QModbusTcpClient>();
        m_client->setTimeout(bounded_timeout(m_requestTimeoutMs));
        m_client->setNumberOfRetries(0);
    }

    std::unique_ptr<QModbusTcpClient> m_client;
    std::uint32_t m_requestTimeoutMs = 1000;
};

class TransportThread final : public QThread {
public:
    explicit TransportThread(std::uint32_t requestTimeoutMs)
        : m_requestTimeoutMs(requestTimeoutMs) {}

    void reset_state() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ready = false;
        m_failed = false;
        m_error.clear();
    }

    bool wait_until_ready(std::string *error) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, std::chrono::seconds(5),
                           [this] { return m_ready || m_failed; })) {
            if (error) {
                *error = "transport thread did not start";
            }
            return false;
        }
        if (m_failed) {
            if (error) {
                *error = m_error;
            }
            return false;
        }
        return true;
    }

    Transport *transport() const noexcept {
        return m_transport.load(std::memory_order_acquire);
    }

    void stop() noexcept {
        if (!isRunning()) {
            return;
        }
        quit();
        if (!wait(5000)) {
            // The transport is intentionally small and has no unbounded loops.
            // Reaching here means a Qt event loop is wedged. Qt documents
            // terminate() as dangerous: the thread can be stopped while
            // modifying data and cannot unlock mutexes or unwind. It is used
            // only because QThread must not be destroyed while running; treat
            // this as a process-exit backstop, never as a normal close path.
            terminate();
            wait();
        }
    }

protected:
    void run() override {
        set_current_thread_name("modbus-qt");
        try {
            Transport transport(m_requestTimeoutMs);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_transport.store(&transport, std::memory_order_release);
                m_ready = true;
            }
            m_cv.notify_all();
            exec();
            m_transport.store(nullptr, std::memory_order_release);
        } catch (const std::exception &error) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_failed = true;
            m_error = error.what();
            m_cv.notify_all();
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_failed = true;
            m_error = "transport thread failed";
            m_cv.notify_all();
        }
    }

private:
    std::uint32_t m_requestTimeoutMs = 1000;
    std::atomic<Transport *> m_transport{nullptr};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_ready = false;
    bool m_failed = false;
    std::string m_error;
};

} // namespace

struct RealSdk::Impl {
    explicit Impl(std::uint32_t requestTimeoutMs)
        : thread(requestTimeoutMs) {}

    TransportThread thread;
    // Serializes "read the transport pointer + post the functor" against
    // stop(). Without it, stop() could destroy the stack-allocated Transport
    // between transport() and invokeMethod().
    std::mutex dispatchMutex;
    bool stopping = false;
};

RealSdk::RealSdk(std::uint32_t requestTimeoutMs)
    : m_impl(std::make_unique<Impl>(requestTimeoutMs)) {}

RealSdk::~RealSdk() { stop(); }

bool RealSdk::start(std::string *error) {
    if (!QCoreApplication::instance()) {
        if (error) {
            *error = "QCoreApplication instance is required";
        }
        return false;
    }
    if (!m_impl) {
        return false;
    }
    if (m_impl->thread.isRunning()) {
        return true;
    }
    m_impl->thread.reset_state();
    m_impl->thread.start();
    return m_impl->thread.wait_until_ready(error);
}

ErrorCode RealSdk::connect(const TcpEndpoint &endpoint, std::uint32_t timeoutMs,
                           std::string *detail) {
    if (!m_impl) {
        return ErrorCode::NotConnected;
    }
    auto call = std::make_shared<ConnectCall>();
    call->endpoint = endpoint;
    call->timeoutMs = timeoutMs;
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    bool posted = false;
    {
        std::lock_guard<std::mutex> dispatch(m_impl->dispatchMutex);
        if (m_impl->stopping || !m_impl->thread.isRunning()) {
            return ErrorCode::NotConnected;
        }
        Transport *transport = m_impl->thread.transport();
        if (!transport) {
            return ErrorCode::Internal;
        }
        posted = QMetaObject::invokeMethod(
            transport,
            [transport, call, promise] {
                try {
                    call->code =
                        transport->connectSync(call->endpoint, call->timeoutMs,
                                               &call->detail);
                } catch (...) {
                    call->exception = std::current_exception();
                }
                try {
                    promise->set_value();
                } catch (...) {
                    // Only one producer exists; a duplicate set_value is ignored.
                }
            },
            Qt::QueuedConnection);
    }
    if (!posted) {
        return ErrorCode::Internal;
    }
    // The wait is a caller-side safety net, not cancellation. If it expires,
    // the queued lambda may still be inside the operation's nested event loop.
    // Do not post a second operation for the same transport until that
    // invocation has been observed to finish; a nested loop also dispatches
    // queued invokeMethod() calls.
    if (future.wait_for(std::chrono::milliseconds(
            caller_wait_ms(timeoutMs))) != std::future_status::ready) {
        return ErrorCode::Timeout;
    }
    if (call->exception) {
        return ErrorCode::Internal;
    }
    if (detail) {
        *detail = call->detail;
    }
    return call->code;
}

ErrorCode RealSdk::disconnect(std::uint32_t timeoutMs, std::string *detail) {
    if (!m_impl) {
        return ErrorCode::Ok;
    }
    auto call = std::make_shared<DisconnectCall>();
    call->timeoutMs = timeoutMs;
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    bool posted = false;
    {
        std::lock_guard<std::mutex> dispatch(m_impl->dispatchMutex);
        if (m_impl->stopping || !m_impl->thread.isRunning()) {
            return ErrorCode::Ok;
        }
        Transport *transport = m_impl->thread.transport();
        if (!transport) {
            return ErrorCode::Internal;
        }
        posted = QMetaObject::invokeMethod(
            transport,
            [transport, call, promise] {
                try {
                    call->code =
                        transport->disconnectSync(call->timeoutMs, &call->detail);
                } catch (...) {
                    call->exception = std::current_exception();
                }
                try {
                    promise->set_value();
                } catch (...) {
                }
            },
            Qt::QueuedConnection);
    }
    if (!posted) {
        return ErrorCode::Internal;
    }
    // The wait is a caller-side safety net, not cancellation. If it expires,
    // the queued lambda may still be inside the operation's nested event loop.
    // Do not post a second operation for the same transport until that
    // invocation has been observed to finish; a nested loop also dispatches
    // queued invokeMethod() calls.
    if (future.wait_for(std::chrono::milliseconds(
            caller_wait_ms(timeoutMs))) != std::future_status::ready) {
        return ErrorCode::Timeout;
    }
    if (call->exception) {
        return ErrorCode::Internal;
    }
    if (detail) {
        *detail = call->detail;
    }
    return call->code;
}

ErrorCode RealSdk::read(const ReadRequest &request, std::uint32_t timeoutMs,
                        ReadResponse *response, std::string *detail) {
    if (!m_impl) {
        return ErrorCode::NotConnected;
    }
    auto call = std::make_shared<ReadCall>();
    call->request = request;
    call->timeoutMs = timeoutMs;
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    bool posted = false;
    {
        std::lock_guard<std::mutex> dispatch(m_impl->dispatchMutex);
        if (m_impl->stopping || !m_impl->thread.isRunning()) {
            return ErrorCode::NotConnected;
        }
        Transport *transport = m_impl->thread.transport();
        if (!transport) {
            return ErrorCode::Internal;
        }
        posted = QMetaObject::invokeMethod(
            transport,
            [transport, call, promise] {
                try {
                    call->code =
                        transport->readSync(call->request, call->timeoutMs,
                                            &call->response, &call->detail);
                } catch (...) {
                    call->exception = std::current_exception();
                }
                try {
                    promise->set_value();
                } catch (...) {
                }
            },
            Qt::QueuedConnection);
    }
    if (!posted) {
        return ErrorCode::Internal;
    }
    // The wait is a caller-side safety net, not cancellation. If it expires,
    // the queued lambda may still be inside the operation's nested event loop.
    // Do not post a second operation for the same transport until that
    // invocation has been observed to finish; a nested loop also dispatches
    // queued invokeMethod() calls.
    if (future.wait_for(std::chrono::milliseconds(
            caller_wait_ms(timeoutMs))) != std::future_status::ready) {
        return ErrorCode::Timeout;
    }
    if (call->exception) {
        return ErrorCode::Internal;
    }
    if (detail) {
        *detail = call->detail;
    }
    if (call->code == ErrorCode::Ok && response) {
        *response = std::move(call->response);
    }
    return call->code;
}

ErrorCode RealSdk::write(const WriteRequest &request, std::uint32_t timeoutMs,
                         WriteResponse *response, std::string *detail) {
    if (!m_impl) {
        return ErrorCode::NotConnected;
    }
    auto call = std::make_shared<WriteCall>();
    call->request = request;
    call->timeoutMs = timeoutMs;
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    bool posted = false;
    {
        std::lock_guard<std::mutex> dispatch(m_impl->dispatchMutex);
        if (m_impl->stopping || !m_impl->thread.isRunning()) {
            return ErrorCode::NotConnected;
        }
        Transport *transport = m_impl->thread.transport();
        if (!transport) {
            return ErrorCode::Internal;
        }
        posted = QMetaObject::invokeMethod(
            transport,
            [transport, call, promise] {
                try {
                    call->code =
                        transport->writeSync(call->request, call->timeoutMs,
                                             &call->response, &call->detail);
                } catch (...) {
                    call->exception = std::current_exception();
                }
                try {
                    promise->set_value();
                } catch (...) {
                }
            },
            Qt::QueuedConnection);
    }
    if (!posted) {
        return ErrorCode::Internal;
    }
    // The wait is a caller-side safety net, not cancellation. If it expires,
    // the queued lambda may still be inside the operation's nested event loop.
    // Do not post a second operation for the same transport until that
    // invocation has been observed to finish; a nested loop also dispatches
    // queued invokeMethod() calls.
    if (future.wait_for(std::chrono::milliseconds(
            caller_wait_ms(timeoutMs))) != std::future_status::ready) {
        return ErrorCode::Timeout;
    }
    if (call->exception) {
        return ErrorCode::Internal;
    }
    if (detail) {
        *detail = call->detail;
    }
    if (call->code == ErrorCode::Ok && response) {
        *response = call->response;
    }
    return call->code;
}

LifeState RealSdk::state() const {
    if (!m_impl) {
        return LifeState::IDLE;
    }
    auto call = std::make_shared<StateCall>();
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    bool posted = false;
    {
        std::lock_guard<std::mutex> dispatch(m_impl->dispatchMutex);
        if (m_impl->stopping || !m_impl->thread.isRunning()) {
            return LifeState::IDLE;
        }
        Transport *transport = m_impl->thread.transport();
        if (!transport) {
            return LifeState::IDLE;
        }
        posted = QMetaObject::invokeMethod(
            transport,
            [transport, call, promise] {
                try {
                    switch (transport->state()) {
                    case QModbusDevice::UnconnectedState:
                        call->value = LifeState::IDLE;
                        break;
                    case QModbusDevice::ConnectingState:
                        call->value = LifeState::CONNECTING;
                        break;
                    case QModbusDevice::ConnectedState:
                        call->value = LifeState::READY;
                        break;
                    case QModbusDevice::ClosingState:
                        call->value = LifeState::RECOVERING;
                        break;
                    }
                } catch (...) {
                    call->exception = std::current_exception();
                }
                try {
                    promise->set_value();
                } catch (...) {
                }
            },
            Qt::QueuedConnection);
    }
    if (!posted) {
        return LifeState::IDLE;
    }
    if (future.wait_for(std::chrono::milliseconds(kStateWaitMs)) !=
        std::future_status::ready) {
        return LifeState::IDLE;
    }
    if (call->exception) {
        return LifeState::IDLE;
    }
    return call->value;
}

void RealSdk::stop() noexcept {
    if (!m_impl) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->dispatchMutex);
        if (m_impl->stopping) {
            return;
        }
        m_impl->stopping = true;
    }
    m_impl->thread.stop();
}

} // namespace modbusscope::detail
