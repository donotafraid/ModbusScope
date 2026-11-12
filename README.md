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
| `ChannelId` | stable id for a dynamic read object |
| `ScopeModel` | pImpl polling worker and ring buffers |
| `ScopeSource` | injectable transport seam (`ClientScopeSource` in production) |
| `ScopeSnapshot` | immutable copy published to the UI thread, plus `readTaskCount` |
| `ScopePresentation` | pure `ScopeSnapshot -> label/color/affordances` mapping |

`ScopeModel::addChannel()` / `removeChannel()` edit the channel list while the
model is `Paused` or `Stopped`. `pause()` is a barrier: it waits for a poll
already in flight, so channel mutation never races `ScopeSource::read()`.
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
- `Client::Impl` is owned by `std::shared_ptr` and derives from
  `std::enable_shared_from_this`. Every blocking entry point takes a snapshot
  before it uses the implementation, so a shutdown that times out cannot free
  the Impl under an in-flight call; the last caller performs the deferred
  transport stop.
- `state()` reads an atomic snapshot only. It does not take the call lock and it
  cannot detect a half-open TCP connection by itself.
- The watchdog thread observes the transport and reconnects within
  `recoveryWindowMs`; after the window it moves to `GIVEN_UP`.
- The Scope worker is a plain `std::thread`. It only calls the thread-safe
  `Client`; the UI thread only reads `ScopeSnapshot` copies. No Qt object is
  touched from the Scope worker.
- `ScopeModel::pause()` is a barrier. It waits for `m_polling` to become false
  before returning; `addChannel()` / `removeChannel()` are accepted only while
  paused or stopped. The worker copies the channel list under the model mutex
  and checks `m_configVersion` before publishing, so a late read is discarded
  rather than attached to a changed list.

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
  the drain deadline expires it returns `IN_FLIGHT_TIMEOUT`; the in-flight call
  keeps the Impl alive and performs the deferred transport stop when it returns,
  so destruction cannot free state under a live call or terminate the process.
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
exposes Start / Pause / Resume / Clear / Add read object. While paused or
stopped, Add opens a dialog for function code 0x03/0x04, address offset and unit
id; every read object is a button with a delete key. `--paused` starts the demo
in the editable paused state (useful for screenshots).

Channels that share `(function, unitId)` and have contiguous addresses are
merged into one Modbus request (up to 125 registers); duplicate addresses share a
single value. The demo status bar reports `reads=` so the effect of adding or
deleting a read object is visible. `ScopeReadPlan` is a pure function with unit
tests, so a gap, unit/function change, or the 125-register ceiling cannot
silently merge unrelated addresses.

The UI state mapping is intentionally two-stage:

| Model state | Transport | UI label | Start | Pause | Resume | Edit |
|---|---|---|---|---|---|---|
| `Stopped` | any | `Stopped` | yes | no | no | yes |
| `Running` | `READY` | `Live` | no | yes | no | no |
| `Running` | `IDLE`/`CONNECTING` before first sample | `Starting` | no | yes | no | no |
| `Running` | `IDLE`/`CONNECTING`/`RECOVERING` | `Idle`/`Connecting`/`Recovering` | no | yes | no | no |
| `Running` | `GIVEN_UP`/`CLOSED` | `Given up`/`Closed` | depends | yes | no | no |
| `Paused` | any | `Paused` | no | no | yes | yes |
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

Then use the script entry points (they configure on first use, unset the libc++
environment variables, and start their own Modbus server):

```sh
./scripts/run_all.sh                  # build + CTest + integration clients
./scripts/run_tests.sh                # same test matrix, no GUI launch
./scripts/run_scope.sh                # local server + Scope window
./scripts/run_scope.sh --screenshot scope.png
./scripts/run_scope.sh --paused --screenshot scope-paused.png
./scripts/run_sanitizers.sh all       # ASan, LSan, TSan
```

The manual configure is still available:

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

CTest registers 60 cases:

- gtest: configuration and request validation, error mapping, the Client state
  machine with `MockSdk`, timeout escalation, recovery/GIVEN_UP, shutdown drain,
  the shutdown-timeout lifetime regression, multi-caller serialization, the
  Qt-free codec, the Scope read planner, ring buffers, dynamic
  add/remove, pause barriers and the full `ScopePresentation` table.
- `ScopeWidget` offscreen render smoke test.
- no-`QCoreApplication` guard, refused-vs-timeout mapping, Rule-of-Five /
  encapsulation probe, write-echo fault injection.
- installed-package consumer (`install_consumer`): installs the tree into a
  throwaway prefix and builds an out-of-tree `find_package(modbusScope)` consumer
  that forces a real library symbol.

`./scripts/run_tests.sh` runs CTest and then the integration clients:
`modbus_client_smoke`, `modbus_client_stress` (8 threads) and
`modbus_client_recovery` against `tests/modbus_server.py --drop-after 3`.
Use `--no-integration` on machines where a local listen socket is forbidden.

Sanitizers:

```sh
./scripts/run_sanitizers.sh asan   # ASan + UBSan, leak detection off
./scripts/run_sanitizers.sh lsan   # LSan with tests/lsan.supp
./scripts/run_sanitizers.sh tsan   # TSan, setarch -R when available
```

`ScopeWidget` is included in the LSan run now. The Qt `offscreen` plugin keeps
process-lifetime allocations; `tests/lsan.supp` suppresses only those shared
libraries, so allocations in the statically linked project are still checked.

`scripts/quickstart.sh` installs missing Qt/Clang packages, runs the Conan
install, configures, builds and runs CTest. It is the fresh-machine path;
`run_all.sh` is the day-to-day path.

## Install and consume

```sh
cmake --install build --prefix /opt/modbusscope
```

```cmake
find_package(modbusScope 0.2 CONFIG REQUIRED)
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
- `requestTimeoutMs` and `connectTimeoutMs` must be at least 10 ms. Both are
  capped at 600 s per attempt; a larger configured value is treated as 600 s.
- No configuration hot reload; `ClientConfig` is a read-only snapshot.
- No automatic write retry. A retry policy belongs to the caller.
- The half-open detector needs traffic or OS keepalive; it is not a heartbeat.
- A shutdown that exceeds the drain deadline returns `IN_FLIGHT_TIMEOUT` and
  leaves the transport to the last in-flight call. If that call never returns,
  the Impl remains alive by design instead of being freed under the call.
- Scope merges only contiguous addresses in the same `(function, unitId)` group;
  it does not read across gaps to reduce request count further.
- See [docs/limitations.md](docs/limitations.md) for the remaining P3 items.
