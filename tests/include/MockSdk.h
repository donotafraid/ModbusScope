#pragma once
#include "modbusscope/detail/Isdk.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace modbusscope::test {

// Deterministic ISdk for state-machine tests. Every method is safe to call
// from the test thread and the Client watchdog thread.
class MockSdk final : public detail::ISdk {
public:
    bool start(std::string *error) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_started = m_startCode == ErrorCode::Ok;
        if (!m_started && error) {
            *error = "mock start failed";
        }
        return m_started;
    }

    ErrorCode connect(const TcpEndpoint &, std::uint32_t,
                      std::string *detail) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_connectCalls;
        if (m_connectCode == ErrorCode::Ok) {
            m_state = LifeState::READY;
        } else {
            m_state = LifeState::IDLE;
            if (detail) {
                *detail = "mock connect failure";
            }
        }
        return m_connectCode;
    }

    ErrorCode disconnect(std::uint32_t, std::string *) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_disconnectCalls;
        if (m_disconnectCode == ErrorCode::Ok) {
            m_state = LifeState::IDLE;
        }
        return m_disconnectCode;
    }

    ErrorCode read(const ReadRequest &request, std::uint32_t,
                   ReadResponse *response, std::string *detail) override {
        const int delayMs = m_readDelayMs.load(std::memory_order_acquire);
        if (delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_readCalls;
        if (m_readCode != ErrorCode::Ok) {
            if (detail) {
                *detail = "mock read failure";
            }
            return m_readCode;
        }
        if (response) {
            response->function = request.function;
            response->registers.assign(request.quantity, m_readValue);
        }
        return ErrorCode::Ok;
    }

    ErrorCode write(const WriteRequest &request, std::uint32_t,
                    WriteResponse *response, std::string *detail) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_writeCalls;
        if (m_writeCode != ErrorCode::Ok) {
            if (detail) {
                *detail = "mock write failure";
            }
            return m_writeCode;
        }
        if (response) {
            response->function = request.function;
            response->startAddress = request.startAddress;
            response->quantity = request.quantity;
        }
        return ErrorCode::Ok;
    }

    LifeState state() const noexcept override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state;
    }

    void stop() noexcept override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_started = false;
        m_state = LifeState::IDLE;
        ++m_stopCalls;
    }

    void setStartCode(ErrorCode code) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_startCode = code;
    }
    void setConnectCode(ErrorCode code) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connectCode = code;
    }
    void setDisconnectCode(ErrorCode code) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_disconnectCode = code;
    }
    void setReadCode(ErrorCode code) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_readCode = code;
    }
    void setWriteCode(ErrorCode code) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_writeCode = code;
    }
    void setReadValue(std::uint16_t value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_readValue = value;
    }
    void setReadDelayMs(int delayMs) {
        m_readDelayMs.store(delayMs, std::memory_order_release);
    }

    unsigned connectCalls() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_connectCalls;
    }
    unsigned disconnectCalls() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_disconnectCalls;
    }
    unsigned readCalls() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_readCalls;
    }
    unsigned writeCalls() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_writeCalls;
    }
    unsigned stopCalls() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stopCalls;
    }

private:
    mutable std::mutex m_mutex;
    LifeState m_state = LifeState::IDLE;
    ErrorCode m_startCode = ErrorCode::Ok;
    ErrorCode m_connectCode = ErrorCode::Ok;
    ErrorCode m_disconnectCode = ErrorCode::Ok;
    ErrorCode m_readCode = ErrorCode::Ok;
    ErrorCode m_writeCode = ErrorCode::Ok;
    std::uint16_t m_readValue = 0;
    bool m_started = false;
    unsigned m_connectCalls = 0;
    unsigned m_disconnectCalls = 0;
    unsigned m_readCalls = 0;
    unsigned m_writeCalls = 0;
    unsigned m_stopCalls = 0;
    std::atomic<int> m_readDelayMs{0};
};

} // namespace modbusscope::test
