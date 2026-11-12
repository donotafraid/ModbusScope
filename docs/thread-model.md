# Thread model

This document records the thread-affinity rules the implementation relies on.
The authority is the Qt 5.15 documentation and the qtserialbus 5.15 source, not
an assumption about `QModbusTcpClient` being thread-safe.

## Rules

1. `QObject` and all subclasses are not thread-safe. An event-driven object may
   only be used from the thread that owns it.
2. `QModbusTcpClient`, its private `QTcpSocket`, every `QModbusReply`, and the
   `QTimer`s created by the client live on the transport thread.
3. Public calls never touch those objects directly. They post a functor with
   `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` and wait on a
   `std::future` with a deadline.
4. `QModbusReply::deleteLater()` runs on the transport thread, after the nested
   `QEventLoop` in `wait_for_reply()` has returned.
5. The transport must be stopped and joined before its `QThread` is destroyed.
   `TransportThread::stop()` calls `quit()` and `wait()`; `terminate()` is the
   last resort for a wedged event loop.
6. The caller must keep the public `Client` object alive while another thread is
   entering one of its methods. The implementation is different: `Client::Impl`
   is held by `std::shared_ptr` and every blocking entry takes a
   `shared_from_this()` snapshot, so an in-flight call outlives a concurrent
   `~Client()` even when the shutdown drain expires.

## Object ownership

```
Client (caller-owned, non-copyable, non-movable)
 └─ std::shared_ptr<Client::Impl>       enable_shared_from_this<Impl>
     ├─ ClientConfig m_config            value snapshot, read-only
     ├─ detail::RealSdk m_sdk
     │   └─ RealSdk::Impl
     │       ├─ std::mutex dispatchMutex guards transport pointer + posting
     │       └─ TransportThread          QThread, owns the event loop
     │           └─ Transport            created inside TransportThread::run()
     │               ├─ QModbusTcpClient
     │               │   └─ QTcpSocket
     │               └─ std::unique_ptr<QModbusTcpClient>
     ├─ std::thread m_watchdog           "modbus-watchdog" (Linux)
     ├─ m_callMutex                      serializes public calls
     └─ m_lifeMutex / m_idleCv           shutdown lease accounting
```

The `shared_ptr` is not a substitute for the caller's ownership of the `Client`
wrapper. It exists because in-flight calls and the shutdown drain have different
lifetimes: a drain timeout must not delete the Impl while `ISdk::read()` is still
using the transport.

`ClientConfig` is copied in `Client::Impl`'s constructor. `endpoint.host` is a
`std::string`, so no `const char*` from the caller is retained. There is no
configuration mutation API; a snapshot cannot be invalidated while a call runs.

## Serialization and deadlines

`read()`, `write()`, `connect()` and `disconnect()` take `m_callMutex` for the
whole blocking operation. The watchdog uses `try_lock()` so it never waits behind
a slow request. `state()` does not take the lock; it only loads the atomic
`m_life`.

`RealSdk::read()` builds a `ReadCall` on the heap, captures the `shared_ptr` in
the queued lambda, and waits at most `requestTimeoutMs + 2 s`. If the wait times
out, the caller returns `TIMEOUT` and never reads the call's output. The lambda
may still run later; it writes only into the heap object it owns.

`RealSdk::stop()` and every dispatch path share `dispatchMutex`. The mutex covers
only "read the transport pointer and post the functor"; it is released before the
caller waits on the future. Without it, `stop()` could destroy the stack-allocated
`Transport` between `thread.transport()` and `QMetaObject::invokeMethod()`.

## Shutdown drain and Impl lifetime

```
Client::shutdown
  -> call_once(shutdown_impl)
     -> m_terminated = true
     -> join watchdog
     -> wait max(requestTimeoutMs, connectTimeoutMs) + 3 s for m_inFlight == 0
        -> drained:    stop_transport_once(); CLOSED
        -> not drained: stopTransportWhenIdle = true; CLOSED; return IN_FLIGHT_TIMEOUT
  -> the in-flight call still owns a shared_ptr<Impl>
     -> it returns, releases its Lease (m_inFlight -> 0)
     -> the last Lease calls stop_transport_once()
     -> it releases its shared_ptr; Impl is destroyed on that thread
```

`stop_transport_once()` is a `std::call_once`, so the drained path, the deferred
Lease path, and `~Impl()` cannot stop the SDK twice. `~Client()` moves its
`shared_ptr` into a local before calling `shutdown()`, which is what lets the
deferred path own the last reference.

## Thread names

On Linux, `watchdog_loop()` sets `modbus-watchdog` and `TransportThread::run()`
sets `modbus-qt` with `pthread_setname_np`, so `top -H` and debugger thread lists
show which thread is which. Other platforms currently leave the default name;
adding `SetThreadDescription` is a P3 item.

## Scope worker

`ScopeModel` adds one more thread, but it never owns a Qt object:

```
ScopeModel::Impl
 ├─ std::thread m_worker      polls ScopeSource::read()
 ├─ ScopeSource m_source      production: ClientScopeSource -> Client
 ├─ std::mutex m_mutex        guards channels, ring buffers, run state, error
 ├─ std::condition_variable m_cv         interval/pause/stop wakeups
 └─ std::condition_variable m_pollingCv  pause barrier (m_polling -> false)
```

The UI thread calls `ScopeModel::snapshot()`, which copies the guarded state,
including `readTaskCount` and stable channel ids. The worker calls
`Client::read()`, which is already serialized and thread-safe by the core client.
`ScopeWidget::Impl` runs a `QTimer` on the GUI thread and paints the copied
snapshot; the worker never calls `QWidget`, `QPainter` or `QTimer`.

The channel list is mutable only in `Paused` or `Stopped`. `pause()` sets
`m_paused`, then waits on `m_pollingCv` until `m_polling` is false. The worker
sets `m_polling` before copying `m_channels` and keeps it true through
`publish_success()` / `publish_error()`, so a pause cannot return in the middle
of a read or publish. `addChannel()` / `removeChannel()` hold `m_controlMutex` and
`m_mutex`; `publish_*` also checks `m_configVersion` and discards stale results.

`ScopeModel::stop()` sets the stop flag, notifies the worker wait path, joins the
worker, then calls `ScopeSource::stop()`. Joining before stopping the source
avoids a worker using a half-destroyed `Client`.

## Official references

- Threads and QObjects:
  <https://doc.qt.io/archives/qt-5.15/threads-qobject.html>
  - "QObject and all of its subclasses are not thread-safe."
  - "Event driven objects may only be used in a single thread."
  - "You must ensure that all objects created in a thread are deleted before you
    delete the QThread."
- QCoreApplication:
  <https://doc.qt.io/archives/qt-5.15/qcoreapplication.html>
- QModbusClient:
  <https://doc.qt.io/archives/qt-5.15/qmodbusclient.html>
- QTimer (GUI-thread refresh):
  <https://doc.qt.io/archives/qt-5.15/qtimer.html>
- QWidget::paintEvent:
  <https://doc.qt.io/archives/qt-5.15/qwidget.html#paintEvent>
- qtserialbus source:
  `src/serialbus/qmodbusclient.cpp`, `src/serialbus/qmodbustcpclient_p.h`
  <https://code.qt.io/cgit/qt/qtserialbus.git/plain/src/serialbus/qmodbusclient.cpp?h=5.15>
