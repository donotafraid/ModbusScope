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
6. The caller must keep the `Client` alive while another thread is inside one of
   its methods. The class does not extend its own lifetime.

## Object ownership

```
Client (caller-owned, non-copyable, non-movable)
 └─ Client::Impl
     ├─ ClientConfig m_config            value snapshot, read-only
     ├─ detail::RealSdk m_sdk
     │   └─ RealSdk::Impl
     │       └─ TransportThread          QThread, owns the event loop
     │           └─ Transport            created inside TransportThread::run()
     │               ├─ QModbusTcpClient
     │               │   └─ QTcpSocket
     │               └─ std::unique_ptr<QModbusTcpClient>
     ├─ std::thread m_watchdog
     ├─ m_callMutex                      serializes public calls
     └─ m_lifeMutex / m_idleCv           shutdown lease accounting
```

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

## Scope worker

`ScopeModel` adds one more thread, but it never owns a Qt object:

```
ScopeModel::Impl
 ├─ std::thread m_worker      polls ScopeSource::read()
 ├─ ScopeSource m_source      production: ClientScopeSource -> Client
 ├─ std::mutex m_mutex        guards ring buffers, run state, error
 └─ std::condition_variable   pause/stop wakeups
```

The UI thread calls `ScopeModel::snapshot()`, which copies the guarded state. The
worker calls `Client::read()`, which is already serialized and thread-safe by the
core client. `ScopeWidget::Impl` runs a `QTimer` on the GUI thread and paints the
copied snapshot; the worker never calls `QWidget`, `QPainter` or `QTimer`.

`ScopeModel::stop()` sets the stop flag, notifies the condition variable, joins
the worker, then calls `ScopeSource::stop()`. Joining before stopping the source
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
