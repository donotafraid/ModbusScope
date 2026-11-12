# SDK call paths (Qt 5.15 / qtserialbus 5.15)

The diagrams below trace the actual calls in this repository. Qt source paths
are relative to the `qtserialbus` repository root.

## 1. Create and connect

```
Client::create
  -> validate_config
  -> new Client(config)                  // ClientConfig copied by value
     -> Impl(config)
        -> RealSdk(requestTimeoutMs)
           -> TransportThread(requestTimeoutMs)
  -> Impl::start_transport
     -> RealSdk::start
        -> QCoreApplication::instance()  // required, else SDK_ERROR
        -> QThread::start
           -> TransportThread::run       [transport thread]
              -> Transport ctor
                 -> new QModbusTcpClient
                 -> setTimeout(requestTimeoutMs)
                 -> setNumberOfRetries(0)
              -> exec()                  // transport event loop
  -> Client::connect
     -> Impl::connect                    // m_callMutex
        -> RealSdk::connect
           -> QMetaObject::invokeMethod(transport, ..., QueuedConnection)
           -> future.wait_for(requestTimeoutMs + 2 s)
              [transport thread]
              -> Transport::connectSync
                 -> setConnectionParameter(NetworkAddressParameter)
                 -> setConnectionParameter(NetworkPortParameter)
                 -> setNumberOfRetries(0)      // never write connect budget here
                 -> connectDevice()
                 -> QEventLoop + QTimer(connectTimeoutMs)
                 -> wait for stateChanged(ConnectedState | UnconnectedState)
                 -> configure_connected_socket()
                    -> setTimeout(requestTimeoutMs)
                    -> QTcpSocket::setSocketOption(KeepAliveOption, 1)
              -> call->code = Ok
              -> promise->set_value()
           -> caller resumes
  -> Impl::start_watchdog
     -> std::thread(watchdog_loop)
```

Official references:

- `QModbusDevice::connectDevice()` only *initiates* the connection; final
  success is the `stateChanged(ConnectedState)` signal.
  <https://doc.qt.io/archives/qt-5.15/qmodbusdevice.html>
- `QModbusClient::setTimeout()` is the response timer, not the TCP connect
  timer. "Already active/running timeouts are not affected by such timeout
  duration changes."
  <https://doc.qt.io/archives/qt-5.15/qmodbusclient.html>
- `QAbstractSocket::KeepAliveOption` enables `SO_KEEPALIVE`.
  <https://doc.qt.io/archives/qt-5.15/qabstractsocket.html>

## 2. Read

```
Client::read
  -> Impl::read                          // m_callMutex
     -> validate_read
     -> RealSdk::read
        -> auto call = make_shared<ReadCall>()   // request + response on heap
        -> QMetaObject::invokeMethod(transport, ..., QueuedConnection)
        -> future.wait_for(requestTimeoutMs + 2 s)
           [transport thread]
           -> Transport::readSync
              -> setTimeout(requestTimeoutMs)   // per request, not per connect
              -> sendReadRequest(unit, unitId)
                 -> QModbusClientPrivate::sendRequest
                    -> new QModbusReply(type, serverAddress, client)
                    -> transaction store + QTimer(m_client->timeout())
              -> ReplyGuard guard(reply)            // all exits below
              -> wait_for_reply(reply, requestTimeoutMs)
                 -> QEventLoop
                 -> quit on finished() or QTimer(timeout + 200 ms)
              -> copy result/raw/error into call
           -> promise->set_value()
        -> if future ready: copy call->response only when code == Ok
```

The `QModbusReply` has the `QModbusClient` as its parent. The transaction timer
calls `setError(TimeoutError)` and emits `finished()`; it does not delete the
reply. `ReplyGuard::deleteLater()` is therefore the single release path.

```
Reply state                         Exit
----------------------------------  ------------------------------------------
finished, NoError                   copy result, guard deleteLater()
finished, TimeoutError/ProtocolError copy error, guard deleteLater()
not finished after safety timer     guard deleteLater() on the pending reply
sendReadRequest returned nullptr    no reply, return device error
exception inside readSync           guard destructor deleteLater()
client/transport teardown           parent client destroys the reply
```

Official references:

- "Do not delete the object in the slot connected to this signal. Use
  deleteLater()."
  <https://doc.qt.io/archives/qt-5.15/qmodbusreply.html>
- `QModbusTcpClientPrivate::sendRequest` stores the reply with a transaction id
  and connects `QObject::destroyed` to remove that transaction:
  `src/serialbus/qmodbustcpclient_p.h`.
  <https://code.qt.io/cgit/qt/qtserialbus.git/plain/src/serialbus/qmodbustcpclient_p.h?h=5.15>

## 3. Write and echo validation

```
Client::write
  -> Impl::write
     -> validate_write
     -> RealSdk::write
        -> WriteCall on heap
        -> invokeMethod + future.wait_for
           -> Transport::writeSync
              -> setTimeout(requestTimeoutMs)
              -> 0x06: sendRawRequest(WriteSingleRegister, address, value)
              -> 0x10: sendRawRequest(WriteMultipleRegisters, address,
                                      quantity, byteCount, values)
              -> ReplyGuard
              -> reply->error() == NoError
              -> raw.isValid()
              -> raw.functionCode() == request.function
              -> raw.dataSize() == 4
              -> decodeData(&echoAddress, &echoSecond)
                 -> 0x06: echoAddress == address && echoSecond == value
                 -> 0x10: echoAddress == address && echoSecond == quantity
```

The wrapper uses `sendRawRequest()` for writes so Qt cannot infer a different
function code from `valueCount`. It forces `numberOfRetries(0)`; a write is
never retried by the transport.

## 4. Timeout escalation and half-open recovery

```
read #1 timeout
  -> Impl::read: ++m_consecutiveTimeouts == 1
  -> TIMEOUT, life stays READY

read #2 timeout
  -> ++m_consecutiveTimeouts == 2
  -> enter_recovery()
     -> m_recoveryEnabled = true
     -> m_life = RECOVERING
     -> mark_bad()
     -> m_sdk->disconnect(connectTimeoutMs)   // drop the half-open socket
  -> TIMEOUT

watchdog_tick (every recoveryIntervalMs)
  -> try_lock(m_callMutex)
  -> READY family check
  -> if RECOVERING:
       -> mark_bad()
       -> recoveryWindowMs expired ? GIVEN_UP : connectSync()
       -> connectSync success -> READY
```

A peer that stops replying without FIN leaves `QTcpSocket` in `ConnectedState`,
so a local `state()` check alone cannot detect it. The escalation is driven by
request timeouts; `SO_KEEPALIVE` is a last-resort signal and may take much longer
than `recoveryWindowMs`. See [limitations.md](limitations.md).

## 5. Shutdown

```
Client::shutdown
  -> std::call_once(shutdown_impl)
     -> m_terminated = true
     -> m_recoveryEnabled = false
     -> m_watchdogStop = true; notify
     -> join watchdog
     -> m_idleCv.wait_for(
          max(requestTimeoutMs, connectTimeoutMs) + 2 s + 1 s,
          m_inFlight == 0)
     -> if drained:
          stop_transport_once()
            -> m_sdk->stop()
               -> TransportThread::quit()
               -> QThread::wait(5 s)
               -> if still running: terminate(); wait()
            -> m_life = CLOSED
        else:
          m_stopTransportWhenIdle = true
          m_shutdownTimedOut = true
          m_life = CLOSED
          return IN_FLIGHT_TIMEOUT
  -> IN_FLIGHT_TIMEOUT if drain failed, otherwise success

last in-flight call leaves Impl::read/write
  -> Lease::~Lease decrements m_inFlight to 0
  -> reads m_stopTransportWhenIdle
  -> stop_transport_once()      // safe: the call still owns a shared_ptr<Impl>
  -> shared_ptr<Impl> released; ~Impl() runs on this thread
```

`~Client()` moves `m_impl` into a local before `shutdown()`, so the last
`shared_ptr` belongs to whichever in-flight call is still finishing. The
transport is not stopped under a live SDK call; a `std::thread` destruction
therefore never calls `terminate()`.

Official references:

- "If no event loop is running, events won't be delivered to the object. ...
  Calling deleteLater() won't work either."
  <https://doc.qt.io/archives/qt-5.15/threads-qobject.html>
- "For non-GUI application that uses Qt, there should be exactly one
  QCoreApplication object."
  <https://doc.qt.io/archives/qt-5.15/qcoreapplication.html>

## 6. Scope polling and UI projection

```
ScopeModel::start
  -> ScopeModel::Impl::start
     -> std::thread(worker_loop)
        -> condition_variable.wait_for(pollIntervalMs)
           or wait for !paused / stop
        -> lock m_mutex
        -> copy m_channels + m_configVersion
        -> m_polling = true
        -> unlock
        -> ScopeSource::read(channels)
           -> ClientScopeSource::read
              -> Client::create (first poll only)
                 -> RealSdk::start
                    -> QThread::start -> QEventLoop
              -> plan_scope_reads(channels)
                 -> group by (function, unitId)
                 -> merge contiguous addresses; split at 125 registers
              -> for each run:
                   Client::read(run)
                     -> RealSdk::read
                        -> QMetaObject::invokeMethod(transport, ...)
                        -> QEventLoop in Transport::readSync
                        -> QModbusReply::finished / TimeoutError
              -> values[channel] = registers[channelOffset]
                                    * scale + offset
        -> ScopeModel::Impl::publish_success
           -> lock m_mutex
           -> discard if configVersion changed
           -> append ring buffer, update min/max/latest, ++sequence
        -> m_polling = false; notify m_pollingCv

UI thread
  -> ScopeWidget::Impl::pull       (QTimer, default 100 ms)
     -> ScopeModel::snapshot()
        -> lock m_mutex, copy channels + samples + run/transport/error
           + readTaskCount
     -> present(snapshot)
        -> ScopePresentation { label, color, hint, canStart, ... }
     -> QWidget::update()
        -> paintEvent
```

`ScopeModel::pause()` stores `m_paused`, notifies `m_cv`, and waits on
`m_pollingCv` until the worker is outside both `ScopeSource::read()` and
`publish_*`. The worker may finish one in-flight poll; `publish_success` keeps
the run state `Paused` in that case. `addChannel()` / `removeChannel()` accept
only `Paused` or `Stopped`, mutate `m_channels` + `m_buffers` under `m_mutex`,
recompute `m_readTaskCount`, and bump `m_configVersion`. `ScopeModel::clear()`
drops samples and statistics but keeps the run state. `ScopeModel::stop()` joins
the worker before calling `ScopeSource::stop()`.

The demo keeps one button per `ChannelId`; deleting calls
`removeChannel(id)` instead of an index. The status bar prints
`channels=` and `reads=` so merged read tasks are visible.

The presentation table is the only place where `LifeState` becomes UI text and
button affordances. See [scope.md](scope.md) for the full table.

Official references:

- `QTimer` on the GUI thread:
  <https://doc.qt.io/archives/qt-5.15/qtimer.html>
- `QWidget::paintEvent` and `QPainter`:
  <https://doc.qt.io/archives/qt-5.15/qwidget.html#paintEvent>
  <https://doc.qt.io/archives/qt-5.15/qpainter.html>
- `QModbusReply::finished` / `deleteLater()`:
  <https://doc.qt.io/archives/qt-5.15/qmodbusreply.html>
