#include "modbusscope/client.h"

#include "detail/realSdk.h"
#include "detail/threadName.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace modbusscope {
namespace {

static_assert(!std::is_copy_constructible_v<Client>);
static_assert(!std::is_move_constructible_v<Client>);
static_assert(std::is_nothrow_destructible_v<Client>);

constexpr std::uint16_t kLastAddress = 0xFFFF;

// Consecutive request timeouts before the transport is declared unhealthy.
// A single slow reply must not tear down a healthy connection; repeated
// timeouts are the only portable half-open signal when the peer never sends
// FIN and SO_KEEPALIVE has not fired yet.
constexpr int kTimeoutRecoveryThreshold = 2;

bool validate_config(const ClientConfig &config, std::string *reason) {
    if (config.endpoint.host.empty()) {
        if (reason) *reason = "endpoint.host is empty";
        return false;
    }
    if (config.endpoint.port == 0) {
        if (reason) *reason = "endpoint.port is 0";
        return false;
    }
    if (config.connectTimeoutMs < 10) {
        if (reason) *reason = "connectTimeoutMs must be >= 10";
        return false;
    }
    if (config.requestTimeoutMs < 10) {
        if (reason) *reason = "requestTimeoutMs must be >= 10";
        return false;
    }
    if (config.recoveryIntervalMs == 0) {
        if (reason) *reason = "recoveryIntervalMs must be > 0";
        return false;
    }
    if (config.recoveryWindowMs < config.recoveryIntervalMs) {
        if (reason) *reason = "recoveryWindowMs must be >= recoveryIntervalMs";
        return false;
    }
    return true;
}

bool validate_read(const ReadRequest &request, std::string *reason) {
    if (!is_read_function(request.function)) {
        if (reason) *reason = "read requires function 0x03 or 0x04";
        return false;
    }
    if (request.unitId == 0 || request.unitId > 247) {
        if (reason) *reason = "unitId must be in 1..247";
        return false;
    }
    if (request.quantity == 0 || request.quantity > kMaxReadRegisters) {
        if (reason) *reason = "quantity must be in 1..125";
        return false;
    }
    if (static_cast<std::uint32_t>(request.startAddress) +
            request.quantity - 1 >
        kLastAddress) {
        if (reason) *reason = "register range exceeds 0xFFFF";
        return false;
    }
    return true;
}

bool validate_write(const WriteRequest &request, std::string *reason) {
    if (!is_write_function(request.function)) {
        if (reason) *reason = "write requires function 0x06 or 0x10";
        return false;
    }
    if (request.unitId == 0 || request.unitId > 247) {
        if (reason) *reason = "unitId must be in 1..247";
        return false;
    }
    if (request.values.size() != request.quantity) {
        if (reason) *reason = "values.size() must equal quantity";
        return false;
    }
    if (request.function == FunctionCode::WriteSingleRegister) {
        if (request.quantity != 1) {
            if (reason) *reason = "0x06 requires quantity == 1";
            return false;
        }
    } else if (request.quantity == 0 ||
               request.quantity > kMaxWriteRegisters) {
        if (reason) *reason = "0x10 requires quantity in 1..123";
        return false;
    }
    if (static_cast<std::uint32_t>(request.startAddress) +
            request.quantity - 1 >
        kLastAddress) {
        if (reason) *reason = "register range exceeds 0xFFFF";
        return false;
    }
    return true;
}

RichError::ErrorCode to_rich_error(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:                 return RichError::ErrorCode::OK;
    case ErrorCode::InvalidArgument:    return RichError::ErrorCode::INVALID_ARGUMENT;
    case ErrorCode::NotConnected:       return RichError::ErrorCode::NOT_CONNECTED;
    case ErrorCode::Timeout:            return RichError::ErrorCode::TIMEOUT;
    case ErrorCode::ProtocolError:      return RichError::ErrorCode::SERVICE_FAILED;
    case ErrorCode::ConnectionFailed:   return RichError::ErrorCode::CONNECTION_FAILED;
    case ErrorCode::Closed:             return RichError::ErrorCode::ALREADY_TERMINATED;
    case ErrorCode::Internal:           return RichError::ErrorCode::INTERNAL_ERROR;
    }
    return RichError::ErrorCode::UNKNOWN;
}

RichError make_error(ErrorCode code, std::string context) {
    return RichError{to_rich_error(code), std::move(context)};
}

std::int64_t now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

class Client::Impl : public std::enable_shared_from_this<Client::Impl> {
public:
    Impl(ClientConfig config, std::unique_ptr<detail::ISdk> sdk)
        : m_config(std::move(config)),
          m_sdk(sdk ? std::move(sdk)
                    : std::make_unique<detail::RealSdk>(
                          m_config.requestTimeoutMs)) {}

    ~Impl() { shutdown(); }

    bool start_transport(std::string *error) {
        return m_sdk->start(error);
    }

    Result<Unit, RichError> start_watchdog() noexcept {
        if (m_watchdog.joinable()) {
            return Result<Unit, RichError>::success(Unit{});
        }
        try {
            m_watchdog = std::thread(&Impl::watchdog_loop, this);
        } catch (const std::system_error &error) {
            return Result<Unit, RichError>::error(
                RichError{RichError::ErrorCode::SDK_ERROR, error.what()});
        } catch (...) {
            return Result<Unit, RichError>::error(
                RichError{RichError::ErrorCode::INTERNAL_ERROR,
                          "failed to start watchdog"});
        }
        return Result<Unit, RichError>::success(Unit{});
    }

    Result<Unit, RichError> connect() noexcept {
        const auto keepAlive = shared_from_this();
        (void)keepAlive; // Outlives ~Client() until this call returns.
        Lease lease(this);
        if (!lease.active()) {
            return error(ErrorCode::Closed, "connect: client is closed");
        }
        std::unique_lock<std::mutex> call(m_callMutex);

        const LifeState state = m_life.load(std::memory_order_acquire);
        if (state == LifeState::READY) {
            return Result<Unit, RichError>::success(Unit{});
        }
        if (state == LifeState::CLOSED || state == LifeState::GIVEN_UP) {
            return error(ErrorCode::Closed,
                         "connect: client is closed or given up");
        }

        // A new manual attempt starts a fresh recovery window. Without this,
        // an old failure timestamp can make the next attempt the last one.
        m_badSinceMs = 0;
        m_consecutiveTimeouts = 0;
        m_recoveryEnabled.store(true, std::memory_order_release);
        m_life.store(LifeState::CONNECTING, std::memory_order_release);
        std::string detail;
        const ErrorCode code =
            m_sdk->connect(m_config.endpoint, m_config.connectTimeoutMs, &detail);
        if (stop_deferred()) {
            return error(ErrorCode::Closed,
                         "connect: client shut down while the call was in flight");
        }
        if (code == ErrorCode::Ok) {
            m_life.store(LifeState::READY, std::memory_order_release);
            reset_bad();
            return Result<Unit, RichError>::success(Unit{});
        }

        m_life.store(state == LifeState::RECOVERING ? LifeState::RECOVERING
                                                    : LifeState::IDLE,
                     std::memory_order_release);
        return error(code, detail.empty() ? "connect failed" : detail);
    }

    Result<Unit, RichError> disconnect() noexcept {
        const auto keepAlive = shared_from_this();
        (void)keepAlive;
        Lease lease(this);
        if (!lease.active()) {
            return error(ErrorCode::Closed, "disconnect: client is closed");
        }
        std::unique_lock<std::mutex> call(m_callMutex);

        const LifeState state = m_life.load(std::memory_order_acquire);
        if (state == LifeState::CLOSED || state == LifeState::GIVEN_UP) {
            return error(ErrorCode::Closed,
                         "disconnect: client is closed or given up");
        }

        // Disable recovery before the IDLE early return. connect() can fail and
        // leave IDLE while recovery is still enabled; an explicit disconnect
        // must still stop the watchdog in that state.
        m_recoveryEnabled.store(false, std::memory_order_release);
        m_badSinceMs = 0;
        m_consecutiveTimeouts = 0;
        if (state == LifeState::IDLE) {
            return Result<Unit, RichError>::success(Unit{});
        }

        std::string detail;
        const ErrorCode code = m_sdk->disconnect(m_config.connectTimeoutMs, &detail);
        if (stop_deferred()) {
            return error(ErrorCode::Closed,
                         "disconnect: client shut down while the call was in flight");
        }
        if (code == ErrorCode::Ok) {
            m_life.store(LifeState::IDLE, std::memory_order_release);
            m_badSinceMs = 0;
            return Result<Unit, RichError>::success(Unit{});
        }
        m_life.store(LifeState::RECOVERING, std::memory_order_release);
        return error(code, detail.empty() ? "disconnect failed" : detail);
    }

    Result<ReadResponse, RichError> read(const ReadRequest &request) noexcept {
        const auto keepAlive = shared_from_this();
        (void)keepAlive;
        Lease lease(this);
        if (!lease.active()) {
            return read_error(ErrorCode::Closed, "read: client is closed");
        }
        std::unique_lock<std::mutex> call(m_callMutex);

        if (m_life.load(std::memory_order_acquire) != LifeState::READY) {
            return read_error(ErrorCode::NotConnected,
                              "read: client is not connected");
        }
        std::string reason;
        if (!validate_read(request, &reason)) {
            return read_error(ErrorCode::InvalidArgument, reason);
        }

        ReadResponse response;
        std::string detail;
        const ErrorCode code = m_sdk->read(
            request, m_config.requestTimeoutMs, &response, &detail);
        if (stop_deferred()) {
            return read_error(ErrorCode::Closed,
                              "read: client shut down while the call was in flight");
        }
        if (code == ErrorCode::Ok) {
            m_consecutiveTimeouts = 0;
            if (m_life.load(std::memory_order_acquire) == LifeState::READY) {
                reset_bad();
            }
            return Result<ReadResponse, RichError>::success(std::move(response));
        }

        if (code == ErrorCode::Timeout) {
            if (++m_consecutiveTimeouts >= kTimeoutRecoveryThreshold) {
                m_consecutiveTimeouts = 0;
                enter_recovery();
            }
            return read_error(code, detail.empty() ? "read timed out" : detail);
        }

        if (code == ErrorCode::ConnectionFailed ||
            code == ErrorCode::NotConnected) {
            m_consecutiveTimeouts = 0;
            enter_recovery();
        }
        return read_error(code, detail.empty() ? "read failed" : detail);
    }

    Result<WriteResponse, RichError> write(const WriteRequest &request) noexcept {
        const auto keepAlive = shared_from_this();
        (void)keepAlive;
        Lease lease(this);
        if (!lease.active()) {
            return write_error(ErrorCode::Closed, "write: client is closed");
        }
        std::unique_lock<std::mutex> call(m_callMutex);

        if (m_life.load(std::memory_order_acquire) != LifeState::READY) {
            return write_error(ErrorCode::NotConnected,
                               "write: client is not connected");
        }
        std::string reason;
        if (!validate_write(request, &reason)) {
            return write_error(ErrorCode::InvalidArgument, reason);
        }

        WriteResponse response;
        std::string detail;
        const ErrorCode code = m_sdk->write(
            request, m_config.requestTimeoutMs, &response, &detail);
        if (stop_deferred()) {
            return write_error(ErrorCode::Closed,
                               "write: client shut down while the call was in flight");
        }
        if (code == ErrorCode::Ok) {
            m_consecutiveTimeouts = 0;
            if (m_life.load(std::memory_order_acquire) == LifeState::READY) {
                reset_bad();
            }
            return Result<WriteResponse, RichError>::success(std::move(response));
        }

        if (code == ErrorCode::Timeout) {
            if (++m_consecutiveTimeouts >= kTimeoutRecoveryThreshold) {
                m_consecutiveTimeouts = 0;
                enter_recovery();
            }
            return write_error(code, detail.empty() ? "write timed out" : detail);
        }

        if (code == ErrorCode::ConnectionFailed ||
            code == ErrorCode::NotConnected) {
            m_consecutiveTimeouts = 0;
            enter_recovery();
        }
        return write_error(code, detail.empty() ? "write failed" : detail);
    }

    LifeState life() const noexcept {
        return m_life.load(std::memory_order_acquire);
    }

    // Test-only: one watchdog iteration on the caller's thread. The production
    // watchdog calls the same tick from m_watchdog.
    void pump_watchdog() noexcept { watchdog_tick(); }

    Result<Unit, RichError> shutdown() noexcept {
        std::call_once(m_shutdownOnce, [this] { shutdown_impl(); });
        if (m_shutdownTimedOut.load(std::memory_order_acquire)) {
            return Result<Unit, RichError>::error(
                RichError{RichError::ErrorCode::IN_FLIGHT_TIMEOUT,
                          "shutdown: in-flight calls did not drain"});
        }
        return Result<Unit, RichError>::success(Unit{});
    }

private:
    class Lease {
    public:
        explicit Lease(Impl *impl) : m_impl(impl) {
            std::lock_guard<std::mutex> lock(m_impl->m_lifeMutex);
            if (m_impl->m_terminated.load(std::memory_order_acquire)) {
                return;
            }
            ++m_impl->m_inFlight;
            m_active = true;
        }

        ~Lease() {
            if (!m_active) {
                return;
            }
            bool stopTransport = false;
            {
                std::lock_guard<std::mutex> lock(m_impl->m_lifeMutex);
                if (--m_impl->m_inFlight == 0) {
                    m_impl->m_idleCv.notify_all();
                    // A shutdown that timed out left the SDK running so that
                    // this call could return through a live transport object.
                    // The last lease performs the deferred stop while the
                    // implementation is still alive.
                    stopTransport = m_impl->m_stopTransportWhenIdle.load(
                        std::memory_order_acquire);
                }
            }
            if (stopTransport) {
                m_impl->stop_transport_once();
            }
        }

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&) = delete;
        Lease &operator=(Lease &&) = delete;

        bool active() const noexcept { return m_active; }

    private:
        Impl *m_impl;
        bool m_active = false;
    };

    static Result<ReadResponse, RichError>
    read_error(ErrorCode code, std::string context) {
        return Result<ReadResponse, RichError>::error(
            make_error(code, std::move(context)));
    }

    static Result<WriteResponse, RichError>
    write_error(ErrorCode code, std::string context) {
        return Result<WriteResponse, RichError>::error(
            make_error(code, std::move(context)));
    }

    static Result<Unit, RichError> error(ErrorCode code, std::string context) {
        return Result<Unit, RichError>::error(make_error(code, std::move(context)));
    }

    void mark_bad() noexcept {
        if (m_badSinceMs == 0) {
            m_badSinceMs = now_ms();
        }
    }

    // True when shutdown gave up on the drain and the transport is still
    // running. State transitions are forbidden in that window: m_life must stay
    // terminal, and the call reports ALREADY_TERMINATED.
    bool stop_deferred() const noexcept {
        return m_terminated.load(std::memory_order_acquire) &&
               m_stopTransportWhenIdle.load(std::memory_order_acquire);
    }

    void reset_bad() noexcept { m_badSinceMs = 0; }

    // A request timeout without FIN from the peer leaves QTcpSocket in
    // ConnectedState. Drop that socket before entering RECOVERING so the
    // watchdog performs a real reconnect instead of connectSync() returning
    // Ok on the still-"connected" half-open socket.
    void enter_recovery() noexcept {
        if (m_terminated.load(std::memory_order_acquire)) {
            return;
        }
        m_recoveryEnabled.store(true, std::memory_order_release);
        m_life.store(LifeState::RECOVERING, std::memory_order_release);
        mark_bad();
        try {
            std::string ignored;
            (void)m_sdk->disconnect(m_config.connectTimeoutMs, &ignored);
        } catch (...) {
            // A failed best-effort drop still leaves the state RECOVERING; the
            // watchdog retry path will call connect() again.
        }
    }

    void shutdown_impl() noexcept {
        {
            std::lock_guard<std::mutex> lock(m_lifeMutex);
            m_terminated.store(true, std::memory_order_release);
        }
        m_recoveryEnabled.store(false, std::memory_order_release);
        m_watchdogStop.store(true, std::memory_order_release);
        m_watchdogCv.notify_all();
        if (m_watchdog.joinable() &&
            m_watchdog.get_id() != std::this_thread::get_id()) {
            m_watchdog.join();
        } else if (m_watchdog.joinable()) {
            m_watchdog.detach();
        }

        bool drained = false;
        {
            std::unique_lock<std::mutex> lock(m_lifeMutex);
            // A caller waits at most operationTimeout + kSdkCallWaitGraceMs
            // inside RealSdk. Give the drain one more second for scheduling so
            // a healthy in-flight call is never reported as IN_FLIGHT_TIMEOUT.
            const auto grace =
                std::chrono::milliseconds(
                    std::max<std::uint32_t>(m_config.requestTimeoutMs,
                                            m_config.connectTimeoutMs) +
                    detail::kSdkCallWaitGraceMs + 1000);
            drained = m_idleCv.wait_for(
                lock, grace,
                [this] { return m_inFlight == 0; });
            if (!drained) {
                // Keep the transport alive: an in-flight call is still using
                // IMPL and may be inside the SDK. The last Lease stops it.
                m_stopTransportWhenIdle.store(true, std::memory_order_release);
            }
        }

        m_life.store(LifeState::CLOSED, std::memory_order_release);
        if (!drained) {
            m_shutdownTimedOut.store(true, std::memory_order_release);
            return;
        }
        stop_transport_once();
    }

    // Stops the SDK at most once. Called by the drain-complete path, or by the
    // last Lease when shutdown timed out. m_sdk stays alive because Impl is
    // owned by shared_ptr snapshots taken by the in-flight calls.
    void stop_transport_once() noexcept {
        std::call_once(m_transportStopOnce, [this] {
            m_sdk->stop();
            m_life.store(LifeState::CLOSED, std::memory_order_release);
        });
    }

    void watchdog_loop() noexcept {
        detail::set_current_thread_name("modbus-watchdog");
        std::unique_lock<std::mutex> lock(m_watchdogMutex);
        while (!m_watchdogStop.load(std::memory_order_acquire)) {
            m_watchdogCv.wait_for(
                lock,
                std::chrono::milliseconds(
                    std::max<std::uint32_t>(m_config.recoveryIntervalMs, 1)),
                [this] {
                    return m_watchdogStop.load(std::memory_order_acquire);
                });
            if (m_watchdogStop.load(std::memory_order_acquire)) {
                break;
            }
            lock.unlock();
            try {
                watchdog_tick();
            } catch (...) {
                // The watchdog must never terminate the process. A failed tick
                // leaves the current LifeState untouched and retries later.
            }
            lock.lock();
        }
    }

    void watchdog_tick() {
        std::unique_lock<std::mutex> call(m_callMutex, std::try_to_lock);
        if (!call.owns_lock()) {
            return;
        }
        if (m_terminated.load(std::memory_order_acquire)) {
            return;
        }
        const LifeState state = m_life.load(std::memory_order_acquire);
        if (state == LifeState::CLOSED || state == LifeState::GIVEN_UP) {
            return;
        }
        if (state == LifeState::READY) {
            if (m_sdk->state() == LifeState::READY) {
                reset_bad();
                return;
            }
            m_life.store(LifeState::RECOVERING, std::memory_order_release);
            mark_bad();
        }
        if (!m_recoveryEnabled.load(std::memory_order_acquire)) {
            return;
        }
        const LifeState current = m_life.load(std::memory_order_acquire);
        if (current != LifeState::RECOVERING && current != LifeState::IDLE) {
            return;
        }
        mark_bad();
        if (now_ms() - m_badSinceMs >=
            static_cast<std::int64_t>(m_config.recoveryWindowMs)) {
            m_life.store(LifeState::GIVEN_UP, std::memory_order_release);
            return;
        }

        m_life.store(LifeState::RECOVERING, std::memory_order_release);
        std::string detail;
        const ErrorCode code =
            m_sdk->connect(m_config.endpoint, m_config.connectTimeoutMs, &detail);
        if (code == ErrorCode::Ok) {
            m_life.store(LifeState::READY, std::memory_order_release);
            m_consecutiveTimeouts = 0;
            reset_bad();
        }
    }

    ClientConfig m_config;
    std::unique_ptr<detail::ISdk> m_sdk;

    mutable std::mutex m_callMutex;
    mutable std::mutex m_lifeMutex;
    std::condition_variable m_idleCv;
    int m_inFlight = 0;
    std::atomic<bool> m_terminated{false};
    std::once_flag m_shutdownOnce;

    std::atomic<LifeState> m_life{LifeState::IDLE};
    std::atomic<bool> m_recoveryEnabled{true};
    std::int64_t m_badSinceMs = 0;      // guarded by m_callMutex
    int m_consecutiveTimeouts = 0;      // guarded by m_callMutex
    std::atomic<bool> m_shutdownTimedOut{false};
    std::atomic<bool> m_stopTransportWhenIdle{false};
    std::once_flag m_transportStopOnce;

    std::thread m_watchdog;
    std::mutex m_watchdogMutex;
    std::condition_variable m_watchdogCv;
    std::atomic<bool> m_watchdogStop{false};
};

Client::Client(ClientConfig config, std::unique_ptr<detail::ISdk> sdk)
    : m_impl(std::make_shared<Impl>(std::move(config), std::move(sdk))) {}

Client::~Client() noexcept {
    // Move the shared_ptr out first: in-flight calls keep the Impl alive even
    // if shutdown() times out, and the last caller performs the deferred stop.
    auto impl = std::move(m_impl);
    if (impl) {
        impl->shutdown();
    }
}

void Client::pump_watchdog() noexcept {
    if (m_impl) {
        m_impl->pump_watchdog();
    }
}

Result<std::unique_ptr<Client>, RichError>
Client::create(ClientConfig config) noexcept {
    return create_impl(std::move(config), nullptr);
}

Result<std::unique_ptr<Client>, RichError>
Client::create_impl(ClientConfig config,
                    std::unique_ptr<detail::ISdk> sdk) noexcept {
    try {
        std::string reason;
        if (!validate_config(config, &reason)) {
            return Result<std::unique_ptr<Client>, RichError>::error(
                RichError{RichError::ErrorCode::INVALID_CONFIG, std::move(reason)});
        }

        auto client = std::unique_ptr<Client>(
            new Client(std::move(config), std::move(sdk)));
        if (!client->m_impl->start_transport(&reason)) {
            return Result<std::unique_ptr<Client>, RichError>::error(
                RichError{RichError::ErrorCode::SDK_ERROR,
                          reason.empty() ? "transport start failed" : reason});
        }

        auto connected = client->connect();
        if (connected.is_fail()) {
            return Result<std::unique_ptr<Client>, RichError>::error(
                *connected.get_error());
        }

        auto watchdog = client->m_impl->start_watchdog();
        if (watchdog.is_fail()) {
            return Result<std::unique_ptr<Client>, RichError>::error(
                *watchdog.get_error());
        }

        return Result<std::unique_ptr<Client>, RichError>::success(
            std::move(client));
    } catch (const std::bad_alloc &error) {
        return Result<std::unique_ptr<Client>, RichError>::error(
            RichError{RichError::ErrorCode::INTERNAL_ERROR,
                      std::string("Memory allocation failed: ") + error.what()});
    } catch (const std::system_error &error) {
        return Result<std::unique_ptr<Client>, RichError>::error(
            RichError{RichError::ErrorCode::SDK_ERROR,
                      std::string("System error: ") + error.what()});
    } catch (const std::exception &error) {
        return Result<std::unique_ptr<Client>, RichError>::error(
            RichError{RichError::ErrorCode::INTERNAL_ERROR,
                      std::string("Unexpected error: ") + error.what()});
    } catch (...) {
        return Result<std::unique_ptr<Client>, RichError>::error(
            RichError{RichError::ErrorCode::UNKNOWN, "Unknown error"});
    }
}

Result<Unit, RichError> Client::connect() noexcept {
    auto impl = m_impl;
    if (!impl) {
        return Result<Unit, RichError>::error(
            RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                      "connect: client is null"});
    }
    return impl->connect();
}

Result<Unit, RichError> Client::disconnect() noexcept {
    auto impl = m_impl;
    if (!impl) {
        return Result<Unit, RichError>::error(
            RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                      "disconnect: client is null"});
    }
    return impl->disconnect();
}

Result<ReadResponse, RichError>
Client::read(const ReadRequest &request) noexcept {
    auto impl = m_impl;
    if (!impl) {
        return Result<ReadResponse, RichError>::error(
            RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                      "read: client is null"});
    }
    return impl->read(request);
}

Result<WriteResponse, RichError>
Client::write(const WriteRequest &request) noexcept {
    auto impl = m_impl;
    if (!impl) {
        return Result<WriteResponse, RichError>::error(
            RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                      "write: client is null"});
    }
    return impl->write(request);
}

LifeState Client::state() const noexcept {
    auto impl = m_impl;
    if (!impl) {
        return LifeState::CLOSED;
    }
    return impl->life();
}

Result<Unit, RichError> Client::shutdown() noexcept {
    auto impl = m_impl;
    if (!impl) {
        return Result<Unit, RichError>::success(Unit{});
    }
    return impl->shutdown();
}

} // namespace modbusscope
