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
- `registerCodec.h` is a small helper set, not a full type system. It has no
  float64, byte-order enum beyond `WordOrder64`, or scaling/engineering units.
- No connection pool, no request batching and no pipelining.

## Repository evidence

- CTest covers the standalone probes and the write-echo fault injection. The
  smoke/stress/recovery/P1 integration clients still need a manually started
  `tests/modbus_server.py`.
- `.github/workflows/sanitizers.yml` and `scripts/build_open62541.sh` belong to a
  previous OPC UA project. They are not a working CI for this library.
- `bench/`, `config_folder/`, `conanfile.txt`, `include/concurrentqueue.h`,
  `src/main.cpp` and the empty `src/modbusscope/main.cpp` are leftovers.
- No `install()` consumer test is run in CI, although the CMake export is now
  present and was checked manually.
- There is no MockSdk or virtual clock test, so state-machine transitions are
  covered by real sockets.

## Recommended next steps

1. Replace the leftover GitHub workflow with a Qt 5.15.3 + clang-17 job that
   runs build, CTest, ASan/LSan and TSan.
2. Add a `MockSdk` + virtual clock unit test for every `LifeState` transition and
   for shutdown with an in-flight call.
3. Remove or archive the OPC UA/benchmark/MQTT residue.
4. Add an installed-package consumer probe to CTest.
