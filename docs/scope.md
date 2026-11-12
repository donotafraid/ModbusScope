# Scope view

The Scope view is a Qt Widgets projection of a thread-safe polling model. It is
deliberately split into a Qt-free model and a Qt-only renderer so the state
mapping can be tested without a display.

## Layers

```
ScopeChannelConfig        one register address, scale, offset; ChannelId stable
        |
        v
ScopeModel::Impl         std::thread worker + ring buffers + mutex
        |  snapshot()
        v
ScopeSnapshot            immutable copy: run state, transport, samples, error,
                         readTaskCount, stable channel ids
        |  present()
        v
ScopePresentation        label, color, hint, canStart/canPause/canResume/...
        |
        v
ScopeWidget::Impl        QTimer pull + QPainter
```

`ScopeModel` is a pImpl class. `ScopeWidget` is a second pImpl class. The core
headers (`scope.h`) contain no Qt; `scopeWidget.h` is the only public header that
includes `QWidget`.

## Threading

```
UI thread                           Scope worker thread
---------                           -------------------
QTimer::timeout (100 ms)            ScopeSource::read(channels)
  -> ScopeModel::snapshot()  <mutex>   -> Client::read() (blocking, serialized)
  -> present(snapshot)                -> publish_success / publish_error
  -> QWidget::update()                   -> lock, append, sequence++
```

- The worker never touches a widget.
- The UI never reads `ScopeModel::Impl`; it only reads a copied snapshot.
- `ScopeModel::start()`, `pause()`, `resume()`, `addChannel()`,
  `removeChannel()`, `clear()` and `stop()` can be called from the UI thread.
  `stop()` joins the worker before stopping the source.
- The channel list is editable only in `Paused` or `Stopped`. `pause()` is a
  barrier: it waits until an in-flight `ScopeSource::read()` has published and
  `m_polling` is false. `addChannel()` / `removeChannel()` then mutate
  `m_channels` and `m_buffers` under `m_mutex`.
- The worker copies the channel list and `m_configVersion` under `m_mutex`.
  `publish_success()` / `publish_error()` discard a result whose version does
  not match, so a late poll cannot be attached to a changed channel list.
- `m_polling` also covers the entire publish step, not just the network call.

## State mapping

`ScopeRunState` describes the polling loop; `LifeState` describes the transport.
They are independent. `present()` combines them:

| `ScopeRunState` | `LifeState` | Label | Start | Pause | Resume | Edit | Error |
|---|---|---|---|---|---|---|---|
| `Stopped` | any | `Stopped` | yes | no | no | yes | no |
| `Running` | `READY` | `Live` | no | yes | no | no | no |
| `Running` + no sample | `IDLE`/`CONNECTING` | `Starting` | no | yes | no | no | no |
| `Running` | `IDLE` | `Idle` | no | yes | no | no | yes |
| `Running` | `CONNECTING` | `Connecting` | no | yes | no | no | yes |
| `Running` | `RECOVERING` | `Recovering` | no | yes | no | no | yes |
| `Running` | `GIVEN_UP` | `Given up` | no | yes | no | no | yes |
| `Running` | `CLOSED` | `Closed` | yes | no | no | no | yes |
| `Paused` | any | `Paused` | no | no | yes | yes | from `lastError` |
| `Faulted` | any | `Fault` | yes | yes | no | no | yes |

`ScopeWidget` does not re-implement this table. It calls `present()` and paints
`label`, `color`, `hint` and `showError`. The demo window uses the same function
to enable Add, Start, Pause, Clear and the per-channel delete keys.

## SDK path

```
ScopeModel::start
  -> Impl::start
     -> std::thread(worker_loop)
        -> wait_for(pollIntervalMs) or pause/stop wakeup
        -> copy m_channels + m_configVersion under m_mutex
        -> ScopeSource::read(channels)
           -> ClientScopeSource::read
              -> ensure_client
                 -> Client::create
                    -> RealSdk::start
                       -> QThread + QEventLoop          [transport thread]
              -> plan_scope_reads(channels)
                 -> group by (function, unitId)
                 -> merge contiguous addresses, cap at 125
              -> for each read task:
                   Client::read
                     -> Impl::read (m_callMutex)
                        -> RealSdk::read
                           -> QMetaObject::invokeMethod(transport, ...)
                           -> future.wait_for(requestTimeoutMs + 2 s)
                              [transport thread]
                              -> Transport::readSync
                                 -> QModbusTcpClient::sendReadRequest
                                 -> ReplyGuard + wait_for_reply
              -> values[i] = raw * scale + offset
        -> Impl::publish_success
           -> lock m_mutex
           -> discard if configVersion changed
           -> ring buffer push, min/max/latest, sequence++
```

The UI path is a plain copy under the same mutex:

```
ScopeWidget::Impl::pull
  -> ScopeModel::snapshot
     -> lock m_mutex
     -> copy channels + samples + run/transport/error + readTaskCount
  -> present(snapshot)
  -> QWidget::update
     -> paintEvent
        -> grid, traces, legend, status pill, hover readout
```

The demo uses one `QPushButton` per read object. Its delete `QToolButton` is
visible only when `present(snapshot).canEditChannels` is true. Adding opens a
form for the register function (0x03/0x04), address offset and unit id; the
model assigns a stable `ChannelId`, which the button captures. Deleting calls
`removeChannel(id)` and never relies on an index that may change after a
snapshot refresh.

Official references:

- `QWidget::paintEvent` / `QPainter`:
  <https://doc.qt.io/archives/qt-5.15/qwidget.html#paintEvent>
  <https://doc.qt.io/archives/qt-5.15/qpainter.html>
- `QTimer`:
  <https://doc.qt.io/archives/qt-5.15/qtimer.html>
- Threads and QObjects (UI objects stay on the GUI thread):
  <https://doc.qt.io/archives/qt-5.15/threads-qobject.html>
- `QModbusClient` asynchronous reply flow:
  <https://doc.qt.io/archives/qt-5.15/qmodbusclient.html>
  <https://doc.qt.io/archives/qt-5.15/qmodbusreply.html>

## Sampling semantics

- One channel is one register. `value = registers[channelOffset] * scale + offset`.
- Channels are grouped by `(function, unitId)`; contiguous addresses share one
  request, up to 125 registers. A gap starts a new request. Duplicate addresses
  share one value.
- A successful poll appends one sample per channel and increments `sequence`.
- A failed poll sets `Faulted` and `lastError`; the worker keeps retrying.
- A successful poll clears `lastError` and returns to `Running`.
- `pause()` stops polling but retains samples. It waits for a read already in
  flight before returning. `resume()` continues.
- `addChannel()` appends an empty history; `removeChannel(id)` removes that
  object and its history. Other objects keep their samples and ids.
- Removing every channel leaves a valid empty model. The worker then performs no
  Modbus read until a channel is added; `readTaskCount` is zero.
- `clear()` drops samples and statistics but keeps the run state.
- Ring buffers keep at most `historyCapacity` samples per channel.
- `ScopeSource::read()` returns values in channel order. A size mismatch is a
  fault, not a partial update.
- `ScopeSnapshot::readTaskCount` is the number of requests after grouping, not
  the number of channels. `reads=1` with two adjacent channels means one merged
  request.

## Tests

`tests/unit/scope_model_test.cpp` covers ring capacity, pause/resume, fault and
recovery, clear, stop/restart, invalid configuration, add/remove while
paused/stopped, the pause barrier, removing the last channel and history
retention for untouched channels.

`tests/unit/scope_read_plan_test.cpp` covers contiguous merging, address gaps,
unit/function splits, duplicate addresses, the 125-register ceiling and original
channel order.

`tests/unit/scope_presentation_test.cpp` covers every row of the mapping table,
including the paused-edit affordance.

`tests/unit/scope_widget_test.cpp` creates a `QApplication` with
`QT_QPA_PLATFORM=offscreen`, renders the widget to a `QImage`, and checks the
`Live`, `Paused` and `Stopped` labels plus a paused add/remove repaint.

## Limits

- Channels are merged only when addresses are contiguous; a sparse map still
  costs one request per gap.
- No per-channel timeout; the Client's `requestTimeoutMs` applies to each
  merged request.
- `pause()` is synchronous and can block the GUI thread for one in-flight
  request budget. That is the deliberate price of a race-free channel edit.
- The widget copies the whole visible history on every refresh. For the default
  600 samples x 8 channels this is small; a larger history needs a shared
  snapshot or incremental append.
- The Qt `offscreen` platform plugin keeps process-lifetime allocations. LSan
  runs the widget test with `tests/lsan.supp` suppressing only those libraries;
  project objects are still checked.
