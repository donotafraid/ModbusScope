# modbusScope

A small, thread-safe C++17 wrapper around Qt 5.15 `QModbusTcpClient`.

The wrapper owns one `QModbusTcpClient` on a dedicated `QThread` with its own
event loop, serializes public calls, and exposes blocking `read()` / `write()`
results as `Result<T, RichError>`. The public headers contain no Qt types; the
implementation detail `realSdk.h` is not installed.

## Public surface

| Function | Public API | Limit |
|---|---|---|
| 0x03 Read Holding Registers | `Client::read` | 1..125 registers |
| 0x04 Read Input Registers | `Client::read` | 1..125 registers |
| 0x06 Write Single Register | `Client::write` | exactly 1 register |
| 0x10 Write Multiple Registers | `Client::write` | 1..123 registers |

`read()` / `write()` return `Result<T, RichError>`. `state()` returns
`LifeState` directly: its implementation is an atomic load, so there is no
failure mode to wrap. `Client` is non-copyable and non-movable; all five special
members are explicit.

`registerCodec.h` is Qt-free. Its accessors take
`const std::vector<std::uint16_t>&` and return `std::optional<T>`, so a short PDU
yields `std::nullopt` instead of an out-of-bounds read.

## Preconditions

The process must have exactly one `QCoreApplication` (or `QApplication`) before
calling `Client::create()`. The Qt transport runs a nested `QEventLoop`; without
an application object it cannot start. `create()` checks this and returns
`RichError::SDK_ERROR` immediately instead of blocking.

```cpp
#include "modbusscope/modbusscope.h"

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); // must exist before create()
    modbusscope::ClientConfig config;
    config.endpoint = {"127.0.0.1", 502};
    auto created = modbusscope::Client::create(config);
    if (!created) {
        return 1;
    }
    auto client = std::move(*created.get());
    // ...
}
```

## Thread model

```
caller thread A ─┐
caller thread B ─┼─> Client::Impl (m_callMutex) ─> ISdk ─> QThread event loop
caller thread C ─┘                                      │
                                                        └─> QModbusTcpClient
                                                              │
                                                              └─> QTcpSocket
```

- `QModbusClient` is asynchronous and is not thread-safe. Every `QModbusTcpClient`,
  `QTcpSocket`, `QModbusReply`, `QTimer` and `QEventLoop` is created, used and
  destroyed on the transport thread.
- Public calls are serialized with `m_callMutex`; the caller blocks on a
  `std::future` with a deadline. The transport lambda owns heap-allocated input
  and output (`ReadCall` / `WriteCall`), so a late lambda cannot touch a caller
  stack frame.
- `state()` reads an atomic snapshot only. It does not take the call lock and it
  cannot detect a half-open TCP connection by itself.
- The watchdog thread observes the transport and reconnects within
  `recoveryWindowMs`; after the window it moves to `GIVEN_UP`.

This is safe for many calling threads, but the single TCP connection is
serialized. Do not describe it as high-concurrency throughput.

## Timeouts, recovery and shutdown

- `connectTimeoutMs` is the budget for the outer `QEventLoop` around
  `connectDevice()`. It is **not** written into `QModbusClient::setTimeout`.
- `requestTimeoutMs` is the single-attempt response budget. It is applied before
  every `sendReadRequest()` / `sendRawRequest()` call. The wrapper forces
  `QModbusClient::setNumberOfRetries(0)`, so a write is never retried implicitly.
- Two consecutive request timeouts move the client to `RECOVERING` and drop the
  socket, because a peer that stops replying without sending FIN leaves the
  local socket in `ConnectedState`. `SO_KEEPALIVE` is enabled as a last-resort
  backstop. Detection therefore depends on request traffic or OS keepalive
  settings; it is not instantaneous.
- `shutdown()` stops the watchdog, waits for in-flight calls up to
  `max(requestTimeoutMs, connectTimeoutMs) + 3 s`, then stops the transport. If
  the drain deadline expires it returns `IN_FLIGHT_TIMEOUT` instead of reporting
  success. The transport is still stopped, so destruction cannot terminate the
  process.
- `~Client()` calls `shutdown()`. The caller must not destroy the object while
  another thread is still entering a method on it.

See [docs/thread-model.md](docs/thread-model.md),
[docs/error-codes.md](docs/error-codes.md) and
[docs/sdk-paths.md](docs/sdk-paths.md).

## Build

Qt 5.15.3 is required exactly. Use the system C++ standard library; the Qt
libraries on Ubuntu are built with libstdc++. If your environment exports
`-stdlib=libc++`, unset it before configuring, otherwise `std::string` crosses
the Qt ABI boundary with a different layout.

If Clang selects a GCC installation whose libstdc++ headers are not installed,
point it at the GCC that provides them:

```sh
GCC_LIBDIR=$(dirname "$(gcc -print-file-name=libstdc++.so)")
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_COMPILER=clang++-17 \
      -DCMAKE_C_COMPILER=clang-17 \
      -DCMAKE_CXX_FLAGS="--gcc-install-dir=${GCC_LIBDIR}"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Test

The CTest suite covers the no-`QCoreApplication` guard, refused-vs-timeout error
mapping, the Qt-free codec, the Rule-of-Five/encapsulation probe, and the
write-echo validation (the last one starts its own fault-injecting server).

The integration clients need `tests/modbus_server.py`:

```sh
python3 tests/modbus_server.py --port 15020 &
./build/bin/modbus_client_smoke 127.0.0.1 15020
./build/bin/modbus_client_stress 127.0.0.1 15020 8

python3 tests/modbus_server.py --port 15021 --drop-after 3 &
./build/bin/modbus_client_recovery 127.0.0.1 15021

bash tests/run_p1_regression.sh build
bash tests/run_p2_regression.sh build
```

Sanitizers:

```sh
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_CXX_COMPILER=clang++-17
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1 ./build-asan/bin/modbus_client_smoke 127.0.0.1 15020

cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_CXX_COMPILER=clang++-17
cmake --build build-tsan -j
setarch "$(uname -m)" -R ./build-tsan/bin/modbus_client_stress 127.0.0.1 15020 8
```

## Install and consume

```sh
cmake --install build --prefix /opt/modbusscope
```

```cmake
find_package(modbusScope 0.1 CONFIG REQUIRED)
target_link_libraries(app PRIVATE modbusScope::modbusScope)
```

The exported target carries Qt as `LINK_ONLY` dependencies; consumers do not need
Qt include directories to use the public headers, but the final link still needs
the Qt runtime and serial-bus libraries.

## Limits

- Modbus TCP only. RTU and TLS are not implemented.
- Unit id is validated to 1..247; broadcast (0) is rejected because the wrapper
  waits for a reply.
- `requestTimeoutMs` and `connectTimeoutMs` must be at least 10 ms.
- No configuration hot reload; `ClientConfig` is a read-only snapshot.
- No automatic write retry. A retry policy belongs to the caller.
- The half-open detector needs traffic or OS keepalive; it is not a heartbeat.
- The repository still carries unrelated OPC UA/benchmark scripts and a CI
  workflow for another project. See [docs/limitations.md](docs/limitations.md).
