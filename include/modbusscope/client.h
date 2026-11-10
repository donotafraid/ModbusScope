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
// Destruction: the caller must stop using the object before destroying it.
// ~Client() calls shutdown(), so a concurrent call that is still inside read()
// keeps the object alive only through the caller's own ownership (for example a
// std::shared_ptr); this class does not extend its own lifetime.
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
    Result<Unit, RichError> connect() noexcept;

    // Disconnects and disables automatic recovery. IDLE is a no-op.
    // CLOSED/GIVEN_UP fail.
    Result<Unit, RichError> disconnect() noexcept;

    // Blocking 0x03/0x04 read. Thread-safe.
    // The response is written only on success; on failure the caller's output
    // is untouched because the response is built on the worker thread.
    Result<ReadResponse, RichError> read(const ReadRequest &request) noexcept;

    // Blocking 0x06/0x10 write. Thread-safe; never retried automatically.
    // On success the response has been checked against the request echo.
    Result<WriteResponse, RichError> write(const WriteRequest &request) noexcept;

    // Atomic, non-blocking local state snapshot. It never round-trips to the
    // transport, so it cannot observe a half-open TCP connection by itself;
    // consecutive request timeouts drive RECOVERING instead.
    LifeState state() const noexcept;

    // Idempotent. Waits for in-flight calls, stops the watchdog and the
    // transport thread, then marks CLOSED. Returns IN_FLIGHT_TIMEOUT if the
    // drain deadline expires; the transport is still stopped in that case.
    Result<Unit, RichError> shutdown() noexcept;

private:
    class Impl;
    Client(ClientConfig config, std::unique_ptr<detail::ISdk> sdk);
    static Result<std::unique_ptr<Client>, RichError>
    create_impl(ClientConfig config,
                std::unique_ptr<detail::ISdk> sdk) noexcept;
    void pump_watchdog() noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace modbusscope
