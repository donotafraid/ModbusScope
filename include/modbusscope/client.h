#pragma once
#include <memory>

#include "Rust_error_deal/error_deal.h"
#include "modbusscope/config.h"
#include "modbusscope/request.h"
#include "modbusscope/state.h"

namespace modbusscope {

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
    Result<WriteResponse, RichError> write(const WriteRequest &request) noexcept;

    // Cheap, non-blocking snapshot. CLOSED after shutdown().
    Result<LifeState, RichError> state() const noexcept;

    // Idempotent. Waits for in-flight calls, stops the watchdog and the
    // transport thread, then marks CLOSED.
    Result<Unit, RichError> shutdown() noexcept;

private:
    class Impl;
    explicit Client(ClientConfig config);
    std::unique_ptr<Impl> m_impl;
};

} // namespace modbusscope
