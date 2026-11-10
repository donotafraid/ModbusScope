# Error codes

`Client` exposes `RichError`. `ErrorCode` in `modbusscope/error.h` is the
internal `detail::ISdk` vocabulary and is not included by the aggregate header.

| `RichError::ErrorCode` | Produced by | Meaning | Retryable | Caller action |
|---|---|---|---|---|
| `INVALID_CONFIG` | `Client::create` | Empty host, zero port, timeout < 10 ms, recovery window < interval | No | Fix `ClientConfig` |
| `INVALID_ARGUMENT` | `read` / `write` | Function code, unit id, quantity, address range or `values.size()` is invalid | No | Fix the request |
| `CONNECTION_FAILED` | `create` / `connect` | TCP refused or reset while connecting | Yes, after a delay | Check endpoint and peer |
| `CONNECT_TIMEOUT` | reserved | Kept for source compatibility; the current transport reports a connect *timeout* through `TIMEOUT` | Yes | Check network reachability |
| `TIMEOUT` | `create` / `connect` / `read` / `write` | Connect budget or response budget expired | Yes | Back off; on reads, two consecutive timeouts enter `RECOVERING` |
| `NOT_CONNECTED` | `read` / `write` | LifeState is not `READY` | After reconnect | Call `connect()` or wait for the watchdog |
| `SERVICE_FAILED` | `read` / `write` | Modbus exception, malformed PDU, wrong function/address/quantity echo, response length mismatch | Depends on the exception | Inspect `context()`; do not blind-retry a write |
| `SDK_ERROR` | `create` | No `QCoreApplication`, transport thread did not start, system error | No | Create the application object once, then retry |
| `ALREADY_TERMINATED` | any method | `Client` already shut down, or internal pointer is null | No | Recreate the client |
| `IN_FLIGHT_TIMEOUT` | `shutdown` | Drain deadline expired; transport was still stopped | No | Treat as a failed clean shutdown; inspect the process |
| `INTERNAL_ERROR` | any method | Allocation failure or unexpected exception | Unknown | Log and recreate |
| `UNKNOWN` | fallback | Unclassified failure | Unknown | Log |

## Internal to public mapping

`src/modbusscope/client.cpp` owns the only translation:

```
ErrorCode::Ok                 -> RichError::OK
ErrorCode::InvalidArgument    -> INVALID_ARGUMENT
ErrorCode::NotConnected       -> NOT_CONNECTED
ErrorCode::Timeout            -> TIMEOUT
ErrorCode::ProtocolError      -> SERVICE_FAILED
ErrorCode::ConnectionFailed   -> CONNECTION_FAILED
ErrorCode::Closed             -> ALREADY_TERMINATED
ErrorCode::Internal           -> INTERNAL_ERROR
```

`QModbusDevice::Error` maps first to the internal `ErrorCode`:
`ConnectionError -> ConnectionFailed`, `TimeoutError -> Timeout`,
`ProtocolError/ReadError/WriteError -> ProtocolError`,
`ReplyAbortedError -> NotConnected`, `ConfigurationError -> InvalidArgument`.

A refused TCP connect is not a timeout. It returns `CONNECTION_FAILED`; a connect
that exceeds `connectTimeoutMs` returns `TIMEOUT`. The regression test
`p2_error_mapping` pins this distinction.

## Official references

- `QModbusDevice::Error`:
  <https://doc.qt.io/archives/qt-5.15/qmodbusdevice.html#Error-enum>
- `QModbusClient::timeout`:
  <https://doc.qt.io/archives/qt-5.15/qmodbusclient.html#timeout-prop>
- `QAbstractSocket::SocketError`:
  <https://doc.qt.io/archives/qt-5.15/qabstractsocket.html#SocketError-enum>
