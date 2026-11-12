#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Rust_error_deal/error_deal.h"
#include "modbusscope/config.h"
#include "modbusscope/request.h"
#include "modbusscope/state.h"

namespace modbusscope {

// Stable identity of a Scope read object. Ids are unique for the lifetime of
// one ScopeModel and are never reused, so UI buttons can safely capture them
// across snapshots and channel-list rebuilds.
using ChannelId = std::uint64_t;

// One trace in the Scope view. A channel reads exactly one holding/input
// register per poll and applies value = raw * scale + offset.
struct ScopeChannelConfig {
    std::string name;
    FunctionCode function = FunctionCode::ReadHoldingRegisters;
    std::uint8_t unitId = 1;
    std::uint16_t address = 0;
    double scale = 1.0;
    double offset = 0.0;
};

struct ScopeSample {
    std::int64_t timestampMs = 0;
    double value = 0.0;
};

struct ScopeChannelSnapshot {
    ChannelId id = 0;
    ScopeChannelConfig config;
    std::vector<ScopeSample> samples;
    bool hasValue = false;
    double latest = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
};

// Lifecycle of the polling loop, independent of the transport's LifeState.
//
//   Stopped   no worker thread; no polling.
//   Running   worker is polling; transport may still be connecting/recovering.
//   Paused    worker is alive but does not poll; the ring buffers are retained.
//   Faulted   the last poll failed; the worker keeps retrying.
enum class ScopeRunState : int {
    Stopped = 0,
    Running,
    Paused,
    Faulted,
};

const char *to_string(ScopeRunState state) noexcept;

// Immutable copy published by ScopeModel::snapshot(). The UI thread never
// reads ScopeModel::Impl fields directly.
struct ScopeSnapshot {
    ScopeRunState run = ScopeRunState::Stopped;
    LifeState transport = LifeState::IDLE;
    std::uint64_t sequence = 0;
    std::vector<ScopeChannelSnapshot> channels;
    // Number of Modbus requests needed by the current channel set after
    // grouping by (function, unitId) and merging contiguous addresses.
    std::size_t readTaskCount = 0;
    std::string lastError;
    std::int64_t lastSampleMs = 0;
};

// Pure projection from the model state to UI affordances. Keeping this
// function free of Qt makes the mapping table testable without a display.
struct ScopePresentation {
    const char *label = "";
    const char *color = "#94a3b8"; // #RRGGBB
    const char *hint = "";
    bool canStart = false;
    bool canPause = false;
    bool canResume = false;
    bool canClear = false;
    bool canEditChannels = false;
    bool showError = false;
};

ScopePresentation present(const ScopeSnapshot &snapshot) noexcept;

// Test/production seam for the polling loop. Implementations must be
// thread-safe because ScopeModel calls read() from its worker thread and
// state() from the same thread only, while the UI only sees snapshots.
class ScopeSource {
public:
    virtual ~ScopeSource() = default;

    virtual LifeState state() const noexcept = 0;

    // Returns one scaled value per channel, in channel order. On failure the
    // values vector is not used and lastError is set by ScopeModel.
    virtual Result<std::vector<double>, RichError>
    read(const std::vector<ScopeChannelConfig> &channels) = 0;

    virtual void stop() noexcept = 0;
};

// Thread-safe polling model. The public methods may be called from the UI
// thread; polling happens on a private std::thread. The channel list can be
// changed only while Paused or Stopped. pause() is a barrier: it waits for a
// poll that is already in flight, so add/remove never races a read.
//
// The ScopeModel object itself is not safe to destroy while another thread is
// inside a method. Hold it by std::shared_ptr (ScopeWidget does) or join callers
// before destruction. Do not call pause()/addChannel()/removeChannel() from
// inside ScopeSource::read(): the worker owns m_polling there and pause() would
// wait on itself.
class ScopeModel final {
public:
    // Production entry: creates a Client from config on the worker thread.
    static Result<std::unique_ptr<ScopeModel>, RichError>
    create(ClientConfig config,
           std::vector<ScopeChannelConfig> channels,
           std::uint32_t pollIntervalMs = 250,
           std::size_t historyCapacity = 600) noexcept;

    // Test/embedding entry: caller supplies the transport.
    static Result<std::unique_ptr<ScopeModel>, RichError>
    createWithSource(std::unique_ptr<ScopeSource> source,
                     std::vector<ScopeChannelConfig> channels,
                     std::uint32_t pollIntervalMs = 250,
                     std::size_t historyCapacity = 600) noexcept;

    ~ScopeModel() noexcept;

    ScopeModel(const ScopeModel &) = delete;
    ScopeModel &operator=(const ScopeModel &) = delete;
    ScopeModel(ScopeModel &&) = delete;
    ScopeModel &operator=(ScopeModel &&) = delete;

    // Starts or resumes the worker. Faulted -> Running retries without
    // recreating the model. Calling start() while Running is a no-op.
    Result<Unit, RichError> start() noexcept;

    // Paused retains the samples; the worker does not poll until resume().
    // The call waits for a poll already in progress to publish and quiesce.
    Result<Unit, RichError> pause() noexcept;
    Result<Unit, RichError> resume() noexcept;

    // Adds one read object. Allowed while Paused or Stopped; the returned id
    // remains stable until removeChannel() succeeds. The new channel starts
    // with empty history. Returns INVALID_STATE in any other run state.
    Result<ChannelId, RichError>
    addChannel(ScopeChannelConfig channel) noexcept;

    // Removes one read object and its samples. Allowed while Paused or
    // Stopped. Unknown ids return INVALID_ARGUMENT. Removing the last channel
    // leaves a valid empty model that polls nothing until a channel is added.
    Result<Unit, RichError> removeChannel(ChannelId id) noexcept;

    // Drops samples and statistics, keeps the worker state.
    Result<Unit, RichError> clear() noexcept;

    // Stops the worker, joins it, then stops the source. Idempotent.
    Result<Unit, RichError> stop() noexcept;

    ScopeSnapshot snapshot() const;
    ScopeRunState run_state() const noexcept;
    LifeState transport_state() const noexcept;

private:
    class Impl;
    explicit ScopeModel(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace modbusscope
