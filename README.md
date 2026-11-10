# modbusScope

A small, thread-safe C++17 wrapper around Qt 5.15 `QModbusTcpClient`, plus a
Qt Widgets Scope view for live register traces.

The core library owns one `QModbusTcpClient` on a dedicated `QThread` with its
own event loop, serializes public calls, and exposes blocking `read()` /
`write()` results as `Result<T, RichError>`. The core public headers contain no
Qt types; the optional `scopeWidget.h` is the only header that includes Qt. The
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

The Scope API is also Qt-free:

| Type | Role |
|---|---|
| `ScopeChannelConfig` | one register address plus scale/offset |
| `ScopeModel` | pImpl polling worker and ring buffers |
| `ScopeSource` | injectable transport seam (`ClientScopeSource` in production) |
| `ScopeSnapshot` | immutable copy published to the UI thread |
| `ScopePresentation` | pure `ScopeSnapshot -> label/color/affordances` mapping |

`scopeWidget.h` adds `ScopeWidget`, a `QWidget` that paints a `ScopeSnapshot`.
It is built into the separate `modbusScopeGui` target.

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

UI thread ──> ScopeWidget::Impl (QTimer 100 ms)
                 └─> ScopeModel::snapshot()  [mutex]
Scope worker ──> ScopeModel::Impl (std::thread)
                 └─> ScopeSource::read() ─> Client::read() (serialized)
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
- The Scope worker is a plain `std::thread`. It only calls the thread-safe
  `Client`; the UI thread only reads `ScopeSnapshot` copies. No Qt object is
  touched from the Scope worker.

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
[docs/error-codes.md](docs/error-codes.md),
[docs/sdk-paths.md](docs/sdk-paths.md) and [docs/scope.md](docs/scope.md).

## Scope view

The Scope widget is optional and has no dependency on Qt Charts; it paints the
grid, traces, legend, status pill and hover readout with `QPainter`.

```sh
./build/bin/modbus_scope_demo --host 127.0.0.1 --port 15020 --interval 250
```

Defaults: four holding registers at addresses 0..3, unit id 1, 250 ms poll
interval, 600 samples per channel. The demo starts polling immediately and
exposes Start / Pause / Resume / Clear.

The UI state mapping is intentionally two-stage:

| Model state | Transport | UI label | Start | Pause | Resume | Edit |
|---|---|---|---|---|---|---|
| `Stopped` | any | `Stopped` | yes | no | no | yes |
| `Running` | `READY` | `Live` | no | yes | no | no |
| `Running` | `IDLE`/`CONNECTING` before first sample | `Starting` | no | yes | no | no |
| `Running` | `IDLE`/`CONNECTING`/`RECOVERING` | `Idle`/`Connecting`/`Recovering` | no | yes | no | no |
| `Running` | `GIVEN_UP`/`CLOSED` | `Given up`/`Closed` | depends | yes | no | no |
| `Paused` | any | `Paused` | no | no | yes | no |
| `Faulted` | any | `Fault` | yes | yes | no | no |

`present()` owns this table, so it is unit-tested without a display. The widget
only draws the projection; it never reads `ScopeModel::Impl`.

## Build

Qt 5.15.3 is required exactly. Use the system C++ standard library; the Qt
libraries on Ubuntu are built with libstdc++. If your environment exports
`-stdlib=libc++`, unset it before configuring, otherwise `std::string` crosses
the Qt ABI boundary with a different layout.

GTest is supplied through Conan in this repository. One-time setup:

```sh
conan install . -pr:a=build.profile \
      --output-folder=conan_debug_build --build=missing
```

Then configure with the generated toolchain:

```sh
unset CXXFLAGS LDFLAGS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_COMPILER=clang++-17 \
      -DCMAKE_C_COMPILER=clang-17 \
      -DCMAKE_TOOLCHAIN_FILE=conan_debug_build/conan_toolchain.cmake
cmake --build build -j
ctest --test-dir build --output-on-failure
```

If GTest is not found, configure still succeeds but the gtest unit suite is
skipped with a warning. The integration probes remain available. The GUI can be
disabled with `-DMODBUSSCOPE_BUILD_GUI=OFF`.

If Clang selects a GCC installation whose libstdc++ headers are not installed,
point it at the GCC that provides them:

```sh
GCC_LIBDIR=$(dirname "$(gcc -print-file-name=libstdc++.so)")
# add -DCMAKE_CXX_FLAGS="--gcc-install-dir=${GCC_LIBDIR}" to the configure line
```

## Test

CTest registers 45 cases:

- gtest: configuration and request validation, error mapping, the Client state
  machine with `MockSdk`, timeout escalation, recovery/GIVEN_UP, shutdown drain,
  multi-caller serialization, the Qt-free codec, Scope ring buffers and the
  full `ScopePresentation` table.
- `ScopeWidget` offscreen render smoke test.
- no-`QCoreApplication` guard, refused-vs-timeout mapping, Rule-of-Five /
  encapsulation probe, write-echo fault injection.

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
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_CXX_COMPILER=clang++-17 \
      -DCMAKE_TOOLCHAIN_FILE=conan_debug_build/conan_toolchain.cmake
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan -E ScopeWidget \
      --output-on-failure

cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_CXX_COMPILER=clang++-17 \
      -DCMAKE_TOOLCHAIN_FILE=conan_debug_build/conan_toolchain.cmake
cmake --build build-tsan -j
setarch "$(uname -m)" -R ctest --test-dir build-tsan --output-on-failure
```

`ScopeWidget` is excluded from the LSan run because the Qt `offscreen` platform
plugin keeps process-lifetime allocations. Its rendering is still exercised by
ASan/UBSan and TSan.

`scripts/quickstart.sh` installs missing Qt/Clang packages, runs the Conan
install, configures, builds and runs CTest.

## Install and consume

```sh
cmake --install build --prefix /opt/modbusscope
```

```cmake
find_package(modbusScope 0.1 CONFIG REQUIRED)
target_link_libraries(app PRIVATE modbusScope::modbusScope)   # Qt-free core
target_link_libraries(app PRIVATE modbusScope::modbusScopeGui) # optional widget
```

The exported core target carries Qt as `LINK_ONLY` dependencies; consumers do not
need Qt include directories to use the core headers. The GUI target carries
`Qt5::Widgets` as a public dependency.

## Limits

- Modbus TCP only. RTU and TLS are not implemented.
- Unit id is validated to 1..247; broadcast (0) is rejected because the wrapper
  waits for a reply.
- `requestTimeoutMs` and `connectTimeoutMs` must be at least 10 ms.
- No configuration hot reload; `ClientConfig` is a read-only snapshot.
- No automatic write retry. A retry policy belongs to the caller.
- The half-open detector needs traffic or OS keepalive; it is not a heartbeat.
- Each Scope channel is one register read per poll. There is no block-read
  grouping yet, so N channels mean N serial Modbus requests per sample.
- See [docs/limitations.md](docs/limitations.md) for the remaining P3 items.
