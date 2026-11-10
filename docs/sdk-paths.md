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
     -> if timeout: m_shutdownTimedOut = true
     -> m_sdk->stop()
        -> TransportThread::quit()
        -> QThread::wait(5 s)
        -> if still running: terminate(); wait()
     -> m_life = CLOSED
  -> IN_FLIGHT_TIMEOUT if drain failed, otherwise success
```

`~Client()` calls `shutdown()` and ignores the result. The transport is stopped
even on a failed drain, so `std::thread` destruction cannot call `terminate()`.

Official references:

- "If no event loop is running, events won't be delivered to the object. ...
  Calling deleteLater() won't work either."
  <https://doc.qt.io/archives/qt-5.15/threads-qobject.html>
- "For non-GUI application that uses Qt, there should be exactly one
  QCoreApplication object."
  <https://doc.qt.io/archives/qt-5.15/qcoreapplication.html>
