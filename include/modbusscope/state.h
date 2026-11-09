#pragma once

namespace modbusscope {

// Connection-attempt state. ConnState only describes the local "single flight"
// guard around connect()/disconnect(); it is not the public session state.
enum class ConnState : int {
    Idle,
    Connecting
};

// Public session state.
//
//   IDLE        transport is not connected and no recovery is scheduled.
//   CONNECTING  a connect() call is in progress (or the watchdog is retrying).
//   READY       transport is connected; read()/write() may be issued.
//   RECOVERING  a transport error was observed and the watchdog is retrying.
//   GIVEN_UP    the recovery window expired; the caller must recreate the client.
//   CLOSED      shutdown() completed; terminal.
//
enum class LifeState : int {
    IDLE = 0,
    CONNECTING,
    READY,
    RECOVERING,
    GIVEN_UP,
    CLOSED
};

const char* to_string(LifeState state) noexcept;

// State-machine event. Public only for observability; no public API accepts it.
enum class SessionEvent : int {
    Connect,
    ConnectSucceeded,
    ConnectFailed,
    RequestIssued,
    RequestSucceeded,
    RequestFailed,
    Disconnect,
    Shutdown,
    NullPtr,
    ThreadBusy,
    Unknown
};

const char* to_string(SessionEvent event) noexcept;

} // namespace modbusscope
