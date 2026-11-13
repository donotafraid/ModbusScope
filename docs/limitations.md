# Known limitations

These are deliberate scope limits or follow-up items. They do not block the
implemented single-connection use case, but they must not be presented as
finished features.

## Protocol and transport

- Modbus TCP only. RTU, ASCII, TLS and serial framing are not implemented.
- Function codes 0x03, 0x04, 0x06 and 0x10 only.
- Unit id 0 (broadcast) is rejected because the API waits for a reply.
- A single TCP connection is serialized by `m_callMutex`. Throughput is not
  concurrent, even when many caller threads share the client.
- Every call is synchronous from the caller's point of view. There is no
  async/future public API and no caller-provided deadline.

## Failure detection

- A half-open connection (peer stops replying without FIN) is detected by two
  consecutive request timeouts, then a forced disconnect and reconnect. It is
  not detected by `state()`.
- If there is no request traffic, only OS `SO_KEEPALIVE` can notice a dead peer.
  The default Linux keepalive interval is hours; the client does not tune
  `TCP_KEEPIDLE`, `TCP_KEEPINTVL` or `TCP_KEEPCNT`.
- Worst-case detection time is therefore "two request budgets with traffic, or
  the OS keepalive policy without traffic". This is not a heartbeat.
- A write is never retried automatically. The caller owns any retry policy.

## API and configuration

- `ClientConfig` is an immutable snapshot. No hot reload and no per-call timeout.
- `Client::state()` is a local snapshot; it can be `READY` while the peer is gone.
- `connectTimeoutMs` and `requestTimeoutMs` are capped at 600 s per attempt
  (`bounded_timeout()`). A larger configured value is treated as 600 s; the cap
  is not a separate configuration field.
- `registerCodec.h` is a small helper set, not a full type system. It has no
  float64, byte-order enum beyond `WordOrder64`, or scaling/engineering units.
- No connection pool, no request batching and no pipelining.
- A shutdown drain that exceeds its deadline returns `IN_FLIGHT_TIMEOUT` and
  defers the SDK stop to the last in-flight call. The Client wrapper may be
  destroyed, but the Impl stays alive until that call returns. If the call never
  returns, the Impl is intentionally retained rather than freed under the call.

## Repository evidence

- CTest registers 63 cases in a normal build (62 in a sanitizer build, where the
  installed-package consumer is skipped). The gtest suite uses `MockSdk` for the
  Client state machine and `FakeScopeSource` for the Scope model, including the
  pause barrier and dynamic add/remove. The
  smoke/stress/recovery clients are started by `scripts/run_tests.sh`, which owns
  the Python server lifetime.
- `ScopeWidget` is included in the LSan gate. `tests/lsan.supp` suppresses the
  Qt `offscreen` plugin's process-lifetime libraries only; the statically linked
  project objects are still checked.
- Scope channels are grouped by `(function, unitId)` and merged when their
  addresses are contiguous, up to 125 registers. The planner never reads across
  a gap; `ScopeReadPlan` has its own unit tests. Channels can be added and removed
  only while paused or stopped; `pause()` blocks the caller until an in-flight
  poll finishes.
- `install_consumer` installs into a throwaway prefix and builds an out-of-tree
  `find_package(modbusScope)` consumer, so the export is exercised by CTest.

## Recommended next steps

1. Add a virtual clock to the Client watchdog so recovery-window tests do not
   sleep.
2. Add a WebSocket/RTU transport only if the product needs it; the current facade
   is TCP-specific by design.
3. Replace the Python integration server with a CTest fixture that allocates a
   free port, so parallel CTest runs cannot collide.
4. Add a Windows `SetThreadDescription` implementation for thread names; Linux
   already uses `pthread_setname_np`.
