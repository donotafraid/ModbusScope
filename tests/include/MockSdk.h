#pragma once
#include "modbusscope/detail/Isdk.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
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
        if (m_blockNextRead.exchange(false, std::memory_order_acq_rel)) {
            std::unique_lock<std::mutex> gate(m_gateMutex);
            m_readEntered = true;
            m_gateCv.notify_all();
            m_gateCv.wait(gate, [this] { return m_readReleased; });
            m_readReleased = false;
            m_readEntered = false;
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
        stopCount().fetch_add(1, std::memory_order_relaxed);
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

    // Gate the next read() so a test can hold a call inside the SDK while it
    // shuts the Client down. waitReadEntered() returns false on timeout.
    void blockNextRead() {
        m_blockNextRead.store(true, std::memory_order_release);
    }

    bool waitReadEntered(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_gateMutex);
        return m_gateCv.wait_for(lock, timeout,
                                 [this] { return m_readEntered; });
    }

    void releaseBlockedRead() {
        std::lock_guard<std::mutex> lock(m_gateMutex);
        m_readReleased = true;
        m_gateCv.notify_all();
    }

    // Survives the MockSdk object so a test can assert the deferred stop ran
    // after the last in-flight call released the implementation.
    static std::atomic<unsigned> &stopCount() {
        static std::atomic<unsigned> count{0};
        return count;
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
    std::atomic<bool> m_blockNextRead{false};
    std::mutex m_gateMutex;
    std::condition_variable m_gateCv;
    bool m_readEntered = false;
    bool m_readReleased = false;
};

} // namespace modbusscope::test
