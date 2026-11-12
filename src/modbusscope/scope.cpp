#include "modbusscope/scope.h"

#include "modbusscope/client.h"
#include "detail/scopeReadPlan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace modbusscope {
namespace detail {

ScopeReadPlan plan_scope_reads(
    const std::vector<ScopeChannelConfig> &channels) {
    ScopeReadPlan plan;
    plan.channelRun.assign(channels.size(), 0);
    plan.channelOffset.assign(channels.size(), 0);
    if (channels.empty()) {
        return plan;
    }

    std::vector<std::size_t> order(channels.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    std::sort(order.begin(), order.end(),
              [&channels](std::size_t left, std::size_t right) {
                  const auto &a = channels[left];
                  const auto &b = channels[right];
                  const auto key = [](const ScopeChannelConfig &channel) {
                      return (static_cast<unsigned>(channel.function) << 8) |
                             channel.unitId;
                  };
                  if (key(a) != key(b)) {
                      return key(a) < key(b);
                  }
                  if (a.address != b.address) {
                      return a.address < b.address;
                  }
                  return left < right;
              });

    for (std::size_t index : order) {
        const auto &channel = channels[index];
        bool placed = false;
        if (!plan.runs.empty()) {
            ScopeReadRun &run = plan.runs.back();
            const std::uint32_t runEnd =
                static_cast<std::uint32_t>(run.startAddress) + run.quantity;
            if (run.function == channel.function &&
                run.unitId == channel.unitId &&
                channel.address >= run.startAddress &&
                channel.address < runEnd) {
                // Duplicate or overlapping address: one request serves both.
                plan.channelRun[index] = plan.runs.size() - 1;
                plan.channelOffset[index] =
                    static_cast<std::uint16_t>(channel.address -
                                               run.startAddress);
                run.channels.push_back(index);
                placed = true;
            } else if (run.function == channel.function &&
                       run.unitId == channel.unitId &&
                       channel.address == runEnd &&
                       run.quantity < kMaxReadRegisters) {
                ++run.quantity;
                plan.channelRun[index] = plan.runs.size() - 1;
                plan.channelOffset[index] =
                    static_cast<std::uint16_t>(run.quantity - 1);
                run.channels.push_back(index);
                placed = true;
            }
        }
        if (placed) {
            continue;
        }
        ScopeReadRun run;
        run.function = channel.function;
        run.unitId = channel.unitId;
        run.startAddress = channel.address;
        run.quantity = 1;
        run.channels.push_back(index);
        plan.channelRun[index] = plan.runs.size();
        plan.channelOffset[index] = 0;
        plan.runs.push_back(std::move(run));
    }
    return plan;
}

} // namespace detail

namespace {

constexpr std::uint32_t kMinPollIntervalMs = 1;
constexpr std::uint32_t kMaxPollIntervalMs = 60000;
constexpr std::size_t kMaxHistoryCapacity = 100000;

std::int64_t now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool validate_channel(const ScopeChannelConfig &channel, std::string *reason) {
    if (channel.name.empty()) {
        if (reason) *reason = "channel name must not be empty";
        return false;
    }
    if (!is_read_function(channel.function)) {
        if (reason) *reason = "scope channels must use 0x03 or 0x04";
        return false;
    }
    if (channel.unitId == 0 || channel.unitId > 247) {
        if (reason) *reason = "channel unitId must be in 1..247";
        return false;
    }
    if (!std::isfinite(channel.scale) || !std::isfinite(channel.offset)) {
        if (reason) *reason = "channel scale/offset must be finite";
        return false;
    }
    return true;
}

bool validate_channels(const std::vector<ScopeChannelConfig> &channels,
                       std::uint32_t pollIntervalMs,
                       std::size_t historyCapacity,
                       std::string *reason) {
    if (channels.empty()) {
        if (reason) *reason = "at least one scope channel is required";
        return false;
    }
    for (const auto &channel : channels) {
        if (!validate_channel(channel, reason)) {
            return false;
        }
    }
    if (pollIntervalMs < kMinPollIntervalMs ||
        pollIntervalMs > kMaxPollIntervalMs) {
        if (reason) *reason = "pollIntervalMs must be in 1..60000";
        return false;
    }
    if (historyCapacity == 0 || historyCapacity > kMaxHistoryCapacity) {
        if (reason) *reason = "historyCapacity must be in 1..100000";
        return false;
    }
    return true;
}

// Production source: one Client per ScopeModel. The client is created lazily
// on the worker thread because Client::create performs a blocking connect.
class ClientScopeSource final : public ScopeSource {
public:
    explicit ClientScopeSource(ClientConfig config) : m_config(std::move(config)) {}

    ~ClientScopeSource() override { stop(); }

    LifeState state() const noexcept override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_client ? m_client->state() : LifeState::IDLE;
    }

    Result<std::vector<double>, RichError>
    read(const std::vector<ScopeChannelConfig> &channels) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ensure_client();
        if (!m_client) {
            return Result<std::vector<double>, RichError>::error(
                RichError{RichError::ErrorCode::SDK_ERROR,
                          m_createError.empty() ? "scope client is not available"
                                                : m_createError});
        }

        std::vector<double> values(channels.size(), 0.0);
        const detail::ScopeReadPlan plan = detail::plan_scope_reads(channels);
        for (const auto &run : plan.runs) {
            ReadRequest request;
            request.function = run.function;
            request.unitId = run.unitId;
            request.startAddress = run.startAddress;
            request.quantity = run.quantity;

            auto result = m_client->read(request);
            if (result.is_fail()) {
                const RichError error = *result.get_error();
                const LifeState transport = m_client->state();
                if (transport == LifeState::GIVEN_UP ||
                    transport == LifeState::CLOSED) {
                    reset_client();
                }
                return Result<std::vector<double>, RichError>::error(error);
            }
            const auto &registers = result.get()->registers;
            if (registers.size() != static_cast<std::size_t>(run.quantity)) {
                return Result<std::vector<double>, RichError>::error(
                    RichError{RichError::ErrorCode::SERVICE_FAILED,
                              "scope read returned an unexpected register count"});
            }
            for (std::size_t channelIndex : run.channels) {
                const auto &channel = channels[channelIndex];
                const std::size_t sampleOffset = plan.channelOffset[channelIndex];
                const double value =
                    static_cast<double>(registers[sampleOffset]) * channel.scale +
                    channel.offset;
                if (!std::isfinite(value)) {
                    return Result<std::vector<double>, RichError>::error(
                        RichError{RichError::ErrorCode::SERVICE_FAILED,
                                  "scaled channel value is not finite"});
                }
                values[channelIndex] = value;
            }
        }
        return Result<std::vector<double>, RichError>::success(std::move(values));
    }

    void stop() noexcept override {
        std::lock_guard<std::mutex> lock(m_mutex);
        reset_client();
    }

private:
    void ensure_client() {
        if (m_client) {
            return;
        }
        auto created = Client::create(m_config);
        if (created.is_success()) {
            m_client = std::move(*created.get());
            m_createError.clear();
        } else {
            m_createError = created.get_error()->context().empty()
                                ? "scope client creation failed"
                                : created.get_error()->context();
        }
    }

    void reset_client() noexcept {
        if (m_client) {
            m_client->shutdown();
            m_client.reset();
        }
    }

    mutable std::mutex m_mutex;
    ClientConfig m_config;
    std::unique_ptr<Client> m_client;
    std::string m_createError;
};

} // namespace

const char *to_string(ScopeRunState state) noexcept {
    switch (state) {
    case ScopeRunState::Stopped:
        return "STOPPED";
    case ScopeRunState::Running:
        return "RUNNING";
    case ScopeRunState::Paused:
        return "PAUSED";
    case ScopeRunState::Faulted:
        return "FAULTED";
    }
    return "UNKNOWN";
}

ScopePresentation present(const ScopeSnapshot &snapshot) noexcept {
    ScopePresentation view;
    view.canClear = true;
    view.showError = !snapshot.lastError.empty();

    switch (snapshot.run) {
    case ScopeRunState::Stopped:
        view.label = "Stopped";
        view.color = "#94a3b8";
        view.hint = "Not polling. Start to acquire samples.";
        view.canStart = true;
        view.canEditChannels = true;
        view.showError = false;
        return view;

    case ScopeRunState::Paused:
        view.label = "Paused";
        view.color = "#38bdf8";
        view.hint = "Polling paused; samples are retained.";
        view.canResume = true;
        view.canEditChannels = false;
        return view;

    case ScopeRunState::Faulted:
        view.label = "Fault";
        view.color = "#ef4444";
        view.hint = "The last poll failed; the worker keeps retrying.";
        view.canStart = true;
        view.canPause = true;
        view.canEditChannels = false;
        view.showError = true;
        return view;

    case ScopeRunState::Running:
        break;
    }

    view.canPause = true;
    view.canEditChannels = false;
    if (snapshot.lastSampleMs == 0 &&
        (snapshot.transport == LifeState::IDLE ||
         snapshot.transport == LifeState::CONNECTING)) {
        view.label = "Starting";
        view.color = "#38bdf8";
        view.hint = "Waiting for the first successful poll.";
        view.showError = false;
        return view;
    }
    switch (snapshot.transport) {
    case LifeState::READY:
        view.label = "Live";
        view.color = "#22c55e";
        view.hint = "Polling the transport.";
        break;
    case LifeState::CONNECTING:
        view.label = "Connecting";
        view.color = "#f59e0b";
        view.hint = "The transport is connecting.";
        view.showError = true;
        break;
    case LifeState::IDLE:
        view.label = "Idle";
        view.color = "#f59e0b";
        view.hint = "The transport is not connected; retrying.";
        view.showError = true;
        break;
    case LifeState::RECOVERING:
        view.label = "Recovering";
        view.color = "#f59e0b";
        view.hint = "The transport is recovering; values may be stale.";
        view.showError = true;
        break;
    case LifeState::GIVEN_UP:
        view.label = "Given up";
        view.color = "#ef4444";
        view.hint = "The recovery window expired; restart the model.";
        view.showError = true;
        break;
    case LifeState::CLOSED:
        view.label = "Closed";
        view.color = "#ef4444";
        view.hint = "The transport is closed; restart the model.";
        view.canPause = false;
        view.canStart = true;
        view.showError = true;
        break;
    }
    return view;
}

class ScopeModel::Impl {
public:
    Impl(std::unique_ptr<ScopeSource> source,
         std::vector<ScopeChannelConfig> channels,
         std::uint32_t pollIntervalMs,
         std::size_t historyCapacity)
        : m_source(std::move(source)),
          m_channels(std::move(channels)),
          m_buffers(m_channels.size()),
          m_pollIntervalMs(pollIntervalMs),
          m_historyCapacity(historyCapacity) {
        for (std::size_t index = 0; index < m_buffers.size(); ++index) {
            m_buffers[index].config = m_channels[index];
        }
    }

    ~Impl() { (void)stop(); }

    Result<Unit, RichError> start() noexcept {
        std::lock_guard<std::mutex> control(m_controlMutex);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_worker.joinable()) {
            if (m_run == ScopeRunState::Faulted ||
                m_run == ScopeRunState::Paused) {
                m_run = ScopeRunState::Running;
                m_lastError.clear();
                ++m_sequence;
            }
            m_paused.store(false, std::memory_order_release);
            m_cv.notify_all();
            return Result<Unit, RichError>::success(Unit{});
        }
        m_stop.store(false, std::memory_order_release);
        m_paused.store(false, std::memory_order_release);
        m_run = ScopeRunState::Running;
        m_lastError.clear();
        ++m_sequence;
        try {
            m_worker = std::thread(&Impl::worker_loop, this);
        } catch (const std::system_error &error) {
            m_run = ScopeRunState::Stopped;
            return Result<Unit, RichError>::error(
                RichError{RichError::ErrorCode::SDK_ERROR, error.what()});
        } catch (...) {
            m_run = ScopeRunState::Stopped;
            return Result<Unit, RichError>::error(
                RichError{RichError::ErrorCode::INTERNAL_ERROR,
                          "failed to start the scope worker"});
        }
        return Result<Unit, RichError>::success(Unit{});
    }

    Result<Unit, RichError> pause() noexcept {
        std::lock_guard<std::mutex> control(m_controlMutex);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_run == ScopeRunState::Stopped) {
            return Result<Unit, RichError>::error(
                RichError{RichError::ErrorCode::INVALID_STATE,
                          "pause: scope model is stopped"});
        }
        if (m_run == ScopeRunState::Paused) {
            return Result<Unit, RichError>::success(Unit{});
        }
        m_paused.store(true, std::memory_order_release);
        m_run = ScopeRunState::Paused;
        ++m_sequence;
        m_cv.notify_all();
        return Result<Unit, RichError>::success(Unit{});
    }

    Result<Unit, RichError> resume() noexcept {
        std::lock_guard<std::mutex> control(m_controlMutex);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_run == ScopeRunState::Stopped) {
            return Result<Unit, RichError>::error(
                RichError{RichError::ErrorCode::INVALID_STATE,
                          "resume: scope model is stopped"});
        }
        if (m_run == ScopeRunState::Paused) {
            m_paused.store(false, std::memory_order_release);
            m_run = ScopeRunState::Running;
            ++m_sequence;
            m_cv.notify_all();
        }
        return Result<Unit, RichError>::success(Unit{});
    }

    Result<Unit, RichError> clear() noexcept {
        std::lock_guard<std::mutex> control(m_controlMutex);
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto &buffer : m_buffers) {
            buffer.samples.clear();
            buffer.hasValue = false;
            buffer.latest = 0.0;
            buffer.minimum = 0.0;
            buffer.maximum = 0.0;
        }
        m_lastError.clear();
        m_lastSampleMs = 0;
        ++m_sequence;
        return Result<Unit, RichError>::success(Unit{});
    }

    Result<Unit, RichError> stop() noexcept {
        std::lock_guard<std::mutex> control(m_controlMutex);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_run == ScopeRunState::Stopped && !m_worker.joinable()) {
                m_source->stop();
                return Result<Unit, RichError>::success(Unit{});
            }
            m_stop.store(true, std::memory_order_release);
            m_paused.store(false, std::memory_order_release);
            m_run = ScopeRunState::Stopped;
            ++m_sequence;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
        m_source->stop();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_transport = LifeState::IDLE;
            ++m_sequence;
        }
        return Result<Unit, RichError>::success(Unit{});
    }

    ScopeSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        ScopeSnapshot result;
        result.run = m_run;
        result.transport = m_transport;
        result.sequence = m_sequence;
        result.lastError = m_lastError;
        result.lastSampleMs = m_lastSampleMs;
        result.channels.reserve(m_buffers.size());
        for (const auto &buffer : m_buffers) {
            ScopeChannelSnapshot channel;
            channel.config = buffer.config;
            channel.samples.assign(buffer.samples.begin(),
                                   buffer.samples.end());
            channel.hasValue = buffer.hasValue;
            channel.latest = buffer.latest;
            channel.minimum = buffer.minimum;
            channel.maximum = buffer.maximum;
            result.channels.push_back(std::move(channel));
        }
        return result;
    }

    ScopeRunState run_state() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_run;
    }

    LifeState transport_state() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_transport;
    }

private:
    struct ChannelBuffer {
        ScopeChannelConfig config;
        std::deque<ScopeSample> samples;
        bool hasValue = false;
        double latest = 0.0;
        double minimum = 0.0;
        double maximum = 0.0;
    };

    void worker_loop() noexcept {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(
                    lock,
                    std::chrono::milliseconds(m_pollIntervalMs),
                    [this] {
                        return m_stop.load(std::memory_order_acquire) ||
                               m_paused.load(std::memory_order_acquire);
                    });
                if (m_stop.load(std::memory_order_acquire)) {
                    return;
                }
                if (m_paused.load(std::memory_order_acquire)) {
                    continue;
                }
            }

            try {
                auto result = m_source->read(m_channels);
                const LifeState transport = m_source->state();
                if (result.is_fail()) {
                    publish_error(transport, result.get_error()->context());
                } else if (result.get()->size() != m_buffers.size()) {
                    publish_error(transport,
                                  "source returned a mismatched channel count");
                } else {
                    publish_success(transport, *result.get());
                }
            } catch (const std::exception &error) {
                publish_error(m_transport, error.what());
            } catch (...) {
                publish_error(m_transport, "scope source read threw");
            }
        }
    }

    void publish_success(LifeState transport, const std::vector<double> &values) {
        const std::int64_t timestamp = now_ms();
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop.load(std::memory_order_acquire)) {
            return;
        }
        m_transport = transport;
        for (std::size_t index = 0; index < m_buffers.size(); ++index) {
            auto &buffer = m_buffers[index];
            buffer.samples.push_back(ScopeSample{timestamp, values[index]});
            if (buffer.samples.size() > m_historyCapacity) {
                buffer.samples.pop_front();
            }
            if (!buffer.hasValue) {
                buffer.hasValue = true;
                buffer.latest = values[index];
                buffer.minimum = values[index];
                buffer.maximum = values[index];
            } else {
                buffer.latest = values[index];
                buffer.minimum = std::min(buffer.minimum, values[index]);
                buffer.maximum = std::max(buffer.maximum, values[index]);
            }
        }
        m_lastError.clear();
        m_lastSampleMs = timestamp;
        m_run = m_paused.load(std::memory_order_acquire)
                    ? ScopeRunState::Paused
                    : ScopeRunState::Running;
        ++m_sequence;
    }

    void publish_error(LifeState transport, std::string context) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop.load(std::memory_order_acquire)) {
            return;
        }
        m_transport = transport;
        m_lastError = context.empty() ? "scope poll failed" : std::move(context);
        m_run = m_paused.load(std::memory_order_acquire)
                    ? ScopeRunState::Paused
                    : ScopeRunState::Faulted;
        ++m_sequence;
    }

    std::unique_ptr<ScopeSource> m_source;
    const std::vector<ScopeChannelConfig> m_channels;
    std::vector<ChannelBuffer> m_buffers;
    const std::uint32_t m_pollIntervalMs;
    const std::size_t m_historyCapacity;

    std::mutex m_controlMutex; // serializes start/pause/resume/clear/stop
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
    ScopeRunState m_run = ScopeRunState::Stopped;
    LifeState m_transport = LifeState::IDLE;
    std::uint64_t m_sequence = 0;
    std::string m_lastError;
    std::int64_t m_lastSampleMs = 0;
};

ScopeModel::ScopeModel(std::unique_ptr<Impl> impl) noexcept
    : m_impl(std::move(impl)) {}

ScopeModel::~ScopeModel() noexcept {
    if (m_impl) {
        (void)m_impl->stop();
    }
}

Result<std::unique_ptr<ScopeModel>, RichError>
ScopeModel::create(ClientConfig config,
                   std::vector<ScopeChannelConfig> channels,
                   std::uint32_t pollIntervalMs,
                   std::size_t historyCapacity) noexcept {
    try {
        auto source = std::make_unique<ClientScopeSource>(std::move(config));
        return createWithSource(std::move(source), std::move(channels),
                                pollIntervalMs, historyCapacity);
    } catch (const std::bad_alloc &error) {
        return Result<std::unique_ptr<ScopeModel>, RichError>::error(
            RichError{RichError::ErrorCode::INTERNAL_ERROR,
                      std::string("scope allocation failed: ") + error.what()});
    }
}

Result<std::unique_ptr<ScopeModel>, RichError>
ScopeModel::createWithSource(std::unique_ptr<ScopeSource> source,
                             std::vector<ScopeChannelConfig> channels,
                             std::uint32_t pollIntervalMs,
                             std::size_t historyCapacity) noexcept {
    try {
        if (!source) {
            return Result<std::unique_ptr<ScopeModel>, RichError>::error(
                RichError{RichError::ErrorCode::INVALID_ARGUMENT,
                          "scope source must not be null"});
        }
        std::string reason;
        if (!validate_channels(channels, pollIntervalMs, historyCapacity,
                               &reason)) {
            return Result<std::unique_ptr<ScopeModel>, RichError>::error(
                RichError{RichError::ErrorCode::INVALID_ARGUMENT,
                          std::move(reason)});
        }
        auto impl = std::make_unique<Impl>(
            std::move(source), std::move(channels), pollIntervalMs,
            historyCapacity);
        return Result<std::unique_ptr<ScopeModel>, RichError>::success(
            std::unique_ptr<ScopeModel>(new ScopeModel(std::move(impl))));
    } catch (const std::bad_alloc &error) {
        return Result<std::unique_ptr<ScopeModel>, RichError>::error(
            RichError{RichError::ErrorCode::INTERNAL_ERROR,
                      std::string("scope allocation failed: ") + error.what()});
    } catch (const std::exception &error) {
        return Result<std::unique_ptr<ScopeModel>, RichError>::error(
            RichError{RichError::ErrorCode::INTERNAL_ERROR, error.what()});
    } catch (...) {
        return Result<std::unique_ptr<ScopeModel>, RichError>::error(
            RichError{RichError::ErrorCode::UNKNOWN,
                      "scope creation failed"});
    }
}

Result<Unit, RichError> ScopeModel::start() noexcept {
    return m_impl ? m_impl->start()
                  : Result<Unit, RichError>::error(
                        RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                                  "start: scope model is null"});
}

Result<Unit, RichError> ScopeModel::pause() noexcept {
    return m_impl ? m_impl->pause()
                  : Result<Unit, RichError>::error(
                        RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                                  "pause: scope model is null"});
}

Result<Unit, RichError> ScopeModel::resume() noexcept {
    return m_impl ? m_impl->resume()
                  : Result<Unit, RichError>::error(
                        RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                                  "resume: scope model is null"});
}

Result<Unit, RichError> ScopeModel::clear() noexcept {
    return m_impl ? m_impl->clear()
                  : Result<Unit, RichError>::error(
                        RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                                  "clear: scope model is null"});
}

Result<Unit, RichError> ScopeModel::stop() noexcept {
    return m_impl ? m_impl->stop()
                  : Result<Unit, RichError>::success(Unit{});
}

ScopeSnapshot ScopeModel::snapshot() const {
    return m_impl ? m_impl->snapshot() : ScopeSnapshot{};
}

ScopeRunState ScopeModel::run_state() const noexcept {
    return m_impl ? m_impl->run_state() : ScopeRunState::Stopped;
}

LifeState ScopeModel::transport_state() const noexcept {
    return m_impl ? m_impl->transport_state() : LifeState::IDLE;
}

} // namespace modbusscope
