#pragma once
#include <memory>
#include <utility>

#include "Rust_error_deal/error_deal.h"
#include "modbusscope/config.h"
#include "modbusscope/request.h"
#include "modbusscope/state.h"

#ifdef MODBUSSCOPE_ENABLE_TEST_HOOKS
#include "modbusscope/detail/Isdk.h"
#endif

namespace modbusscope {

namespace detail {
class ISdk;
}

// Thread-safe Modbus TCP client.
//
// The instance may be shared by any number of threads. Calls into the Qt
// transport are serialized on a private worker thread; the public methods block
// the calling thread until the request completes or times out.
//
// Lifetime: the implementation is held by std::shared_ptr and derives from
// std::enable_shared_from_this. Every blocking entry point takes a shared_ptr
// snapshot before it touches the transport, so the implementation outlives a
// concurrent ~Client() even when the shutdown drain deadline expires. The
// public Client object itself is still caller-owned: do not destroy it while
// another thread is calling into it.
class Client final {
public:
    // Unique creation entry. Returns an error instead of throwing, including
    // std::bad_alloc. On success the client is connected and READY.
    static Result<std::unique_ptr<Client>, RichError>
    create(ClientConfig config) noexcept;

#ifdef MODBUSSCOPE_ENABLE_TEST_HOOKS
    // Test-only creation seam: identical lifecycle to create(), but the
    // transport implementation is injected. Tests include
    // modbusscope/detail/Isdk.h before using this overload.
    static Result<std::unique_ptr<Client>, RichError>
    createWithSdk(std::unique_ptr<detail::ISdk> sdk,
                  ClientConfig config) noexcept {
        return create_impl(std::move(config), std::move(sdk));
    }

    // Runs one watchdog iteration on the calling thread. Production builds do
    // not expose this symbol.
    void pumpWatchdogForTest() noexcept { pump_watchdog(); }
#endif

    ~Client() noexcept;

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    Client(Client &&) = delete;
    Client &operator=(Client &&) = delete;

    // Connects if IDLE/RECOVERING. READY is a no-op. CLOSED/GIVEN_UP fail.
    // Returns only after the socket is connected or the attempt timed out.
    // Calls are serialized, so the wall-clock wait can also include another
    // caller's in-flight operation.
    Result<Unit, RichError> connect() noexcept;

    // Disconnects and disables automatic recovery. IDLE is a no-op.
    // CLOSED/GIVEN_UP fail.
    Result<Unit, RichError> disconnect() noexcept;

    // Blocking 0x03/0x04 read. Thread-safe.
    // The response is written only on success; on failure the caller's output
    // is untouched because the response is built on the worker thread.
    // The request budget starts only after this call acquires the transport
    // lock; queued callers can therefore observe a longer wall-clock wait.
    Result<ReadResponse, RichError> read(const ReadRequest &request) noexcept;

    // Blocking 0x06/0x10 write. Thread-safe; never retried automatically.
    // On success the response has been checked against the request echo.
    // Shares the same serialization and latency rule as read().
    Result<WriteResponse, RichError> write(const WriteRequest &request) noexcept;

    // Atomic, non-blocking local state snapshot. It never round-trips to the
    // transport, so it cannot observe a half-open TCP connection by itself;
    // consecutive request timeouts drive RECOVERING instead.
    LifeState state() const noexcept;

    // Idempotent. Waits for in-flight calls, stops the watchdog and the
    // transport thread, then marks CLOSED. Returns IN_FLIGHT_TIMEOUT if the
    // drain deadline expires. In that case the transport is stopped by the
    // last in-flight call when it leaves the implementation, not here; a later
    // shutdown() reports success once that deferred stop has completed.
    Result<Unit, RichError> shutdown() noexcept;

private:
    class Impl;
    Client(ClientConfig config, std::unique_ptr<detail::ISdk> sdk);
    static Result<std::unique_ptr<Client>, RichError>
    create_impl(ClientConfig config,
                std::unique_ptr<detail::ISdk> sdk) noexcept;
    void pump_watchdog() noexcept;

    std::shared_ptr<Impl> m_impl;
};

} // namespace modbusscope
