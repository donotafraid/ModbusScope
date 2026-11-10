#pragma once
#include "modbusscope/scope.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace modbusscope::test {

class FakeScopeSource final : public ScopeSource {
public:
    LifeState state() const noexcept override {
        return m_state.load(std::memory_order_acquire);
    }

    Result<std::vector<double>, RichError>
    read(const std::vector<ScopeChannelConfig> &) override {
        m_polls.fetch_add(1, std::memory_order_relaxed);
        const int delayMs = m_delayMs.load(std::memory_order_acquire);
        if (delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fail) {
            return Result<std::vector<double>, RichError>::error(
                RichError{RichError::ErrorCode::SERVICE_FAILED, m_error});
        }
        return Result<std::vector<double>, RichError>::success(m_values);
    }

    void stop() noexcept override {
        m_stopped.store(true, std::memory_order_release);
    }

    void setValues(std::vector<double> values) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_values = std::move(values);
    }
    void setFailure(bool fail, std::string error = {}) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fail = fail;
        m_error = std::move(error);
    }
    void setTransport(LifeState state) {
        m_state.store(state, std::memory_order_release);
    }
    void setDelayMs(int delayMs) {
        m_delayMs.store(delayMs, std::memory_order_release);
    }

    int polls() const { return m_polls.load(std::memory_order_relaxed); }
    bool stopped() const {
        return m_stopped.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex m_mutex;
    std::vector<double> m_values;
    bool m_fail = false;
    std::string m_error;
    std::atomic<LifeState> m_state{LifeState::READY};
    std::atomic<bool> m_stopped{false};
    std::atomic<int> m_polls{0};
    std::atomic<int> m_delayMs{0};
};

} // namespace modbusscope::test
