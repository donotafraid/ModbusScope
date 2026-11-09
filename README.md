# modbusScope

A small, thread-safe C++17 wrapper around Qt 5.15 `QModbusTcpClient`.

The public headers contain no Qt types. The implementation owns one
`QModbusTcpClient` on a dedicated `QThread` with its own event loop, serializes
public calls, and exposes blocking `read()` / `write()` results as
`Result<T, RichError>`.

## Supported operations

| Function | Public API | Limit |
|---|---|---|
| 0x03 Read Holding Registers | `Client::read` | 1..125 registers |
| 0x04 Read Input Registers | `Client::read` | 1..125 registers |
| 0x06 Write Single Register | `Client::write` | exactly 1 register |
| 0x10 Write Multiple Registers | `Client::write` | 1..123 registers |

The wrapper never retries a write. The Qt client retry count is forced to `0`;
`requestTimeoutMs` is therefore a single-attempt response budget.

## Thread model

```
caller thread A ─┐
caller thread B ─┼─> Client::Impl (mutex) ─> ISdk ─> QThread event loop
caller thread C ─┘                                      │
                                                        └─> QModbusTcpClient
                                                              │
                                                              └─> QTcpSocket
```

`QModbusClient` is asynchronous and its `QModbusReply` objects, `QTimer`s and
`QTcpSocket` all live in the object's thread. The wrapper never calls
`sendReadRequest()` or `delete` on the Qt object from a caller thread. A call is
posted to the transport thread and the caller waits on a `std::future`.

`state()` is an atomic snapshot and does not take the call lock. `connect()`,
`disconnect()`, `read()` and `write()` are serialized. A watchdog thread
observes the transport and retries within `recoveryWindowMs`; after the window
the state becomes `GIVEN_UP` and the caller must recreate the client.

## Build and test

This report was verified with Clang 17 + libc++. If your Clang cannot find
`<memory>`, use the `-stdlib=libc++` flags below (or install the libstdc++
headers matching Clang's default search path).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_COMPILER=clang++-17 \
      -DCMAKE_CXX_FLAGS=-stdlib=libc++ \
      -DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++
cmake --build build -j

python3 tests/modbus_server.py --port 15020 &
./build/bin/modbus_client_smoke 127.0.0.1 15020
./build/bin/modbus_client_stress 127.0.0.1 15020 8
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

## Limits

- Modbus TCP only. RTU and TLS are not implemented.
- Unit id is validated to 1..247; broadcast (0) is rejected because the wrapper
  waits for a reply.
- `requestTimeoutMs` and `connectTimeoutMs` must be at least 10 ms (the Qt
  setter ignores smaller values).
- The caller must keep the `Client` alive while another thread is inside one of
  its methods; `~Client()` waits for in-flight calls but does not extend the
  object's lifetime.
