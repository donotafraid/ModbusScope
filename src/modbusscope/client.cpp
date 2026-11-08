#include <memory>
#include <qobject.h>
#include <qobjectdefs.h>
#include <system_error>
#include <thread>

#include "modbusscope/recreatSync.h"
#include "modbusscope/realSdk.h"
#include <qmodbusdataunit.h>

#include "modbusscope/version.h"
#include "modbusscope/error.h"
#include "Rust_error_deal/error_deal.h"
#include "modbusscope/state.h"
#include "modbusscope/config.h"
#include "modbusscope/request.h"
#include "modbusscope/client.h"

using Client = modbusscope::Client;

struct Client::QModbus_ClientDeleter {
  std::shared_ptr<ISdk> sdk;            // 持有 sdk
  std::shared_ptr<CallbackContext> ctx; // 关键：持有一份 shared_ptr

  void operator()(QModbusClient *c) const noexcept {
    if (c && sdk) {
      // 此期间即使 clientContext 被回调读取，ctx 也一定活着。
      // ISdk::clientDelete 的契约是不抛；若自定义实现违约，这里吞掉异常
      // 以避免 std::shared_ptr 的 noexcept 析构直接 terminate。
      try {
        sdk->clientDelete(c);
      } catch (...) {
      }
    }
    // deleter 作用域结束后 ctx 才释放。
    // 如果 Impl 早死、此处是最后一个引用，CallbackContext 在此销毁——
    // 此时已无任何 UA_Client 能触发回调，安全。
  }
};

struct Client::CallbackContext {
  std::weak_ptr<Impl>
      implWeak; // 注意是 weak，不是
                // shared,确保只有在调用lock()转换为shared_ptr下才改变计数
};

namespace { // 匿名命名空间，实现内部链接
class InFlightGuard {
public:
  enum class Mode { Shared, Exclusive };
  InFlightGuard(std::mutex &taskCvMx, std::condition_variable &taskCv,
                size_t &taskCounter)
      : m_taskCvMx(taskCvMx), m_taskCv(taskCv), m_taskCounter(taskCounter) {
    std::lock_guard<std::mutex> lock(m_taskCvMx);
    ++m_taskCounter;
  }

  ~InFlightGuard() {
    bool shouldNotify = false;
    {
      std::lock_guard<std::mutex> lock(m_taskCvMx);
      if (m_taskCounter > 0) {
        --m_taskCounter;
        if (m_taskCounter == 0)
          shouldNotify = true;
      }
    }
    if (shouldNotify)
      m_taskCv.notify_all();
  }

  // 禁止拷贝
  InFlightGuard(const InFlightGuard &) = delete;
  InFlightGuard &operator=(const InFlightGuard &) = delete;

  // 禁止移动构造
  InFlightGuard(InFlightGuard &&other) = delete;
  // 禁止移动赋值
  InFlightGuard &operator=(InFlightGuard &&) = delete;

private:
  std::mutex &m_taskCvMx;
  std::condition_variable &m_taskCv;
  size_t &m_taskCounter;
};

class CASGuard {
public:
  using ConnState = modbusscope::ConnState;
  CASGuard(std::atomic<ConnState> &connState) : m_connState(connState) {}

  ~CASGuard() { m_connState.store(ConnState::Idle, std::memory_order_release); }

  // 禁止拷贝
  CASGuard(const CASGuard &) = delete;
  CASGuard &operator=(const CASGuard &) = delete;

  // 禁止移动构造
  CASGuard(CASGuard &&other) = delete;
  // 禁止移动赋值
  CASGuard &operator=(CASGuard &&) = delete;

private:
  std::atomic<ConnState> &m_connState;
};

class RecreateGuard {
public:
  using RecreateSync = modbusscope::RecreateSync;
  RecreateGuard(std::shared_ptr<RecreateSync> recreateSync)
      : m_recreateSync(recreateSync) {}

  ~RecreateGuard() {
    std::lock_guard<std::mutex> lock(m_recreateSync->m_recreateLock);
    if (!isReset) {
      isReset = true;
      m_recreateSync->m_recreating.store(false, std::memory_order_release);
      m_recreateSync->m_recreateCv
          .notify_all(); // 持锁 notify，消除 CV 生命周期竞态
    }
  }

  void reset() {
    std::lock_guard<std::mutex> lock(m_recreateSync->m_recreateLock);
    if (!isReset) {
      isReset = true;
      m_recreateSync->m_recreating.store(false, std::memory_order_release);
      m_recreateSync->m_recreateCv
          .notify_all(); // 持锁 notify，消除 CV 生命周期竞态
    }
  }

  // 禁止拷贝
  RecreateGuard(const RecreateGuard &) = delete;
  RecreateGuard &operator=(const RecreateGuard &) = delete;

  // 禁止移动构造
  RecreateGuard(RecreateGuard &&other) = delete;
  // 禁止移动赋值
  RecreateGuard &operator=(RecreateGuard &&) = delete;

private:
  bool isReset = false;
  std::shared_ptr<RecreateSync> m_recreateSync;
};

// Marks the shutdown() call that won the m_terminated race. The destructor
// waits for this counter: without it, a second shutdown() can return success
// and the caller can destroy the object while the winner is still inside
// shutdownInternal() (touching m_recreateSync / pImpl).
class ShutdownCallGuard {
public:
  using RecreateSync = modbusscope::RecreateSync;
  explicit ShutdownCallGuard(
      std::shared_ptr<RecreateSync> recreateSync)
      : m_recreateSync(std::move(recreateSync)) {}

  ~ShutdownCallGuard() {
    std::lock_guard<std::mutex> lock(m_recreateSync->m_recreateLock);
    if (m_recreateSync->m_shutdownInFlight > 0) {
      --m_recreateSync->m_shutdownInFlight;
      if (m_recreateSync->m_shutdownInFlight == 0)
        m_recreateSync->m_recreateCv.notify_all();
    }
  }

  ShutdownCallGuard(const ShutdownCallGuard &) = delete;
  ShutdownCallGuard &operator=(const ShutdownCallGuard &) = delete;
  ShutdownCallGuard(ShutdownCallGuard &&) = delete;
  ShutdownCallGuard &operator=(ShutdownCallGuard &&) = delete;

private:
  std::shared_ptr<RecreateSync> m_recreateSync;
};

class ThreadGuard {
public:
  ThreadGuard(std::unique_ptr<std::thread> watchdogThread,
              std::condition_variable &watchdogCv)
      : m_watchdogThread(std::move(watchdogThread)), m_watchdogCv(watchdogCv) {}

  ~ThreadGuard() {
    if (!m_watchdogThread || !m_watchdogThread->joinable())
      return;
    m_watchdogCv.notify_all();
    // 与 joinWatchdog() 保持同一防御：绝不 join 当前线程。
    // 若析构线程恰好就是看门狗线程，无条件 join 会抛 std::system_error，
    // 而析构函数中抛出异常即 std::terminate。
    try {
      if (m_watchdogThread->get_id() != std::this_thread::get_id()) {
        m_watchdogThread->join();
      } else {
        // Destructor is running on the watchdog thread. Skipping join leaves a
        // joinable std::thread member, whose destructor calls std::terminate.
        // The loop has already exited (we are in ~Impl), so detaching is safe.
        m_watchdogThread->detach();
      }
    } catch (...) {
      if (m_watchdogThread->joinable())
        m_watchdogThread->detach();
    }
  }

  // 禁止拷贝
  ThreadGuard(const ThreadGuard &) = delete;
  ThreadGuard &operator=(const ThreadGuard &) = delete;

  // 禁止移动构造
  ThreadGuard(ThreadGuard &&other) = delete;
  // 禁止移动赋值
  ThreadGuard &operator=(ThreadGuard &&) = delete;

private:
  std::unique_ptr<std::thread> m_watchdogThread;
  std::condition_variable &m_watchdogCv;
};

class ApiLease {
public:
  enum class Reason { Active, Recreating, Terminated };
  using RecreateSync = modbusscope::RecreateSync;
  explicit ApiLease(std::shared_ptr<RecreateSync> recreateSync)
      : s_(recreateSync) {
    {
      std::lock_guard<std::mutex> lock(s_->m_recreateLock);
      if (s_->m_terminated.load(std::memory_order_acquire)) {
        reason_=Reason::Terminated;
        return;
      }
      if (s_->m_recreating.load(std::memory_order_acquire)) {
        reason_=Reason::Recreating;
        return;
      }
      ++s_->m_inFlight;
    }
    reason_ = Reason::Active;
  }
  ~ApiLease() {
    if (reason_!=Reason::Active)
      return;
    {
      std::lock_guard lk(s_->m_recreateLock);
      if (--s_->m_inFlight == 0)
        s_->m_inFlightCv.notify_all();
    }
  }
  ApiLease(const ApiLease &) = delete;
  ApiLease &operator=(const ApiLease &) = delete;
  ApiLease(ApiLease &&) = delete;
  ApiLease &operator=(ApiLease &&) = delete;
  //true:表示API计数成功；fasle:表示API因为m_recreating或m_terminated而失败导致未计数
  bool isActive() { return reason_ == ApiLease::Reason::Active; }
  Reason reason() { return reason_; }

private:
  std::shared_ptr<RecreateSync> s_;
  Reason reason_;
};

}; // namespace

class Client::Impl : public std::enable_shared_from_this<Client::Impl>,
                     public QObject {
  Q_OBJECT
public:
  using QModbus_ClientPtr = std::shared_ptr<QModbusClient>;
  QModbus_ClientPtr m_impl;
  std::shared_ptr<modbusscope::ISdk> m_sdk;
  mutable std::mutex m_lock;
  std::shared_ptr<CallbackContext> m_callbackContext;
  ClientConfig m_config; // configure 后只读，不能再修改
  std::atomic<ConnState> m_connState{
      ConnState::
          Idle}; // CAS状态机，描述此时是否存在线程进行连接/断连操作，connect()进行状态设置
  std::atomic<LifeState> m_lifeState{
      LifeState::RUNNING}; // Client
                           // 存活状态机，描述是否正常存活,看门狗进行状态修改
  std::atomic<bool> m_running{true}; // 看门狗运行状态,shutdown设置，没有重置
  std::atomic<bool> m_shutdown{false}; // 析构运行状态，shutdown设置，没有重置
  std::atomic<bool> m_stop{
      false}; // shutdown()保护变量，避免其他因素导致shutdown()未被调用
  std::mutex m_taskCvMx; // 专门保护任务计数和条件变量
  std::condition_variable m_taskCv;
  size_t m_taskCounter = 0; // 由 m_taskCvMx
  // 保护,当前未完成的读写任务总量，在读写任务开始和结束时修改，用于shutdown()判断是否可以析构当前对象
  // 非递归互斥：stateCallback / inactivityCallback 只碰原子量，不回调任何
  // 会再次获取 m_sdkMutex 的 Impl 方法；若未来回调需要访问加锁方法，必须
  // 改回 recursive_mutex 并在这里说明真实的递归调用链。
  std::mutex
      m_sdkMutex; // 使得SDK 调用真正串行化，connect/disconnect 不再与读写交叠
  // 看门狗辅助
  std::unique_ptr<std::thread> m_watchdogThread;
  std::condition_variable m_watchdogCv;
  mutable std::mutex m_watchDogMx;
  // 延时函数辅助
  std::mutex m_retryCvMx;
  std::condition_variable m_retryCv;
  // give-up 时间窗成员
  std::atomic<int64_t> m_lastBadStatusMs{
      0}; // 自首次观测到非 GOOD 起计时
          // 60s,针对可能的业务停摆（线程卡死/暂停/忘记轮询）超过
          // 60s，如果前状态是GOOD，会触发重连而非直接退出
  std::mutex m_BadStatusMsMx; // 专门保护m_lastBadStatusMs的赋值
  //  initialize function
  Impl(const ClientConfig &cfg, QModbus_ClientPtr client,
       std::shared_ptr<CallbackContext> callbackContext_,
       std::shared_ptr<modbusscope::ISdk> sdk)
      : m_config(cfg), m_impl(std::move(client)),
        m_callbackContext(callbackContext_), m_sdk(sdk) {}

  //   销毁前必须 join：任何直接或间接导致 Impl 析构的路径（如
  //   shutdown()、recreateGiveUpClient() 中的 shutdown()、OPC_UA_Client
  //   析构）都必须先调用 Impl::shutdown() 置位停止标志并 join 看门狗线程。
  // 防御性保障：~Impl() 本身会再次置位停止标志并 join，以防未来新增路径遗漏调用
  // shutdown()。
  // 析构不保证已完成清理
  ~Impl() {
    // 不预先置位 m_shutdown，让 shutdown() 完成断开 + 唤醒看门狗
    if (!m_stop.load(std::memory_order_acquire)) {
      shutdown();
    }
    // 防御性置位：确保看门狗线程能够退出（即使没有显式调用 shutdown()）
    m_shutdown.store(true, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    m_watchdogCv.notify_all(); // 唤醒可能在等待的看门狗
    m_retryCv.notify_all();

    doCleanup();
  }

  // “调用期间禁止修改/析构入参 vector”
  // 单次 SDK 调用阻塞上界 ≈ config.timeout，断线窗口叠加 connectSync
  /**
   * @brief 关于同步服务调用与看门狗重连的交互说明
   *
   * 设计意图：
   *   -
   * 连接状态管理（重连/断连）主要由看门狗线程负责；业务读写线程在检测到连接未就绪
   *     （RECOVERING 或
   * GIVEN_UP）时快速失败，不主动重连，避免业务线程长时间阻塞。
   *
   * SDK 实际行为（open62541 v1.4.14）：
   *   - 任何同步服务调用（如 UA_Client_Service_read/write）在发现 SecureChannel
   * 未打开 或 Session 未激活时，会自动调用 connectSync()
   * 尝试重新建立连接（阻塞至多 config.timeout），然后才继续服务请求。
   *   - noReconnect
   * 仅禁止通道被动关闭后的后台自动重连，不禁止服务调用前的补连； noNewSession
   * 才会在会话丢失时直接中止连接。
   *
   * 实际交互结果：
   *   - 即使在 RECOVERING 状态下，若业务线程已通过状态检查并进入 SDK
   * 调用，也可能触发 SDK 内部的 connectSync 重连。
   *   - 该 connectSync 与看门狗的重连操作通过 SDK 内部 clientMutex
   * 串行化，不会发生双重 connect
   * 或资源竞争，但可能导致业务线程阻塞比预期更长，且重连发起者不一定是看门狗。
   *
   * 结论：
   *   - 本封装“重连权威在
   * watchdog”的表述为近似成立，并非绝对。实际重连可能由读写线程
   *     触发，但最终连接状态仍由看门狗统一管理和恢复。
   *
   */
  Result<Unit,RichError>
  batchRead(const ReadRequest &batchReadNodes);
  // “调用期间禁止修改/析构入参 vector”
  /**
   * @brief 执行批量写入（单次尝试，无内部自动重试）
   *
   * @note 由于 OPC UA 写入操作具有非幂等性（可能导致重复执行），
   *       本接口不对网络瞬断等可恢复错误进行底层重试。
   *       若写入失败（返回 Bad），调用方应根据业务逻辑自行决定是否重试。
   *       如需自动重试，请确保写入操作的幂等性，或在上层业务循环中调用本接口。
   */
  Result<Unit, RichError> batchWrite(const WriteRequest &reqs,
                                     const QVector<quint16> &values);
  // create() 返回后，客户端一定已连接，调用方可以立即使用
  Result<Unit, SessionEvent> connect();
  // 等待连接就绪
  Result<Unit, SessionEvent> disconnect();
  // shutdown():
  //   - 幂等：可随时调用。
  //   - 实际断开由 ~Impl → doCleanup → m_impl.reset() → UA_Client_delete 完成。
  //   - 断开时机取决于最后一个 QModbus_ClientPtr 的释放时机，可能晚于
  //   shutdown() 返回。
  void shutdown() noexcept;
  // 置位 + notify；实际断开由 ~Impl → doCleanup → UA_Client_delete 完成
  void signalShutdown();
  // 最大阻塞时间 = joinWatchdog 阻塞时间 + max(timeoutMs, 2×watchdogIntervalMs)
  void joinWatchdog();
  // clear pointer
  void doCleanup() noexcept;
  /**
   * @brief 配置客户端参数（超时、重试策略等）
   **/
  Result<Unit, RichError> configureClient(bool useDefault);
  Result<Unit, RichError> validateConfiguration(bool useDefault);
  Result<Unit, RichError> applyConfiguration(bool useDefault);
  // 启动看门狗线程
  Result<Unit, RichError> startWatchdog();
  //  延时函数
  void delayFunction(uint32_t retry);
  // 看门狗循环
  // 断开重连退避最长 retryMaxBackoffMs
  void runWatchdog();
  void doWatchdogDecision(std::shared_ptr<Impl> ImplPtr);

  //  辅助函数:计算退避时间
  uint32_t calculateBackoff(uint32_t retry) const;

  // 辅助函数：检查是否应该触发 GIVEN_UP
  // 判据：now - m_lastBadStatusMs >= m_config.giveUpThresholdMs
  // 注意：本函数**不读取 sdkConnectStatus**——这是刻意的，
  //       因为 noReconnect=true 时 SDK 不会自行放弃重连，
  //       "是否放弃"应由封装层的时间窗口决定，而非 SDK 状态。
  // 首次坏状态启动计时，恢复后由 resetGiveUpTimer() 重置
  bool shouldTriggerGiveUp();
  void updateBadStatusTime();
  void resetGiveUpTimer();

  // 辅助函数:LifeState状态切换对应的函数
  void TurnToRunning();
  void TurnToRecovring() noexcept;
  void TurnToGiveup();

  //  辅助函数:返回特定有效对象
  ClientConfig getEffectiveConfig();

  //  辅助函数:返回是否停止运行信号
  // true:停止运行,false:允许继续运行
  // 判据：m_shutdown && !m_running，或 m_lifeState == GIVEN_UP。
  // 注意：GIVEN_UP 也算"停止信号"——这是刻意的，
  // 让 GIVEN_UP 后的所有读写与 connect 立即快速失败。
  bool getEffectiveStopSignal();

  // 辅助函数:返回健康状态
  template <typename Fn> void updatePacked(Fn &&mutate);

  // 判据：四元组全就绪（connectStatus==GOOD && channel==OPEN &&
  // session==ACTIVATED）&& (!connectionLost)  RUNNING 的充要条件即本函数返回
  // true。
  bool isHealthyCached() const;

  // 辅助函数:判断Channel,ConnectStatus,SessionStatus状态是否处于CLOSED，用于判断connect是否合适进行
  bool isChannelClosed() const;
  bool isConnectBad() const;
  bool isSessionClosed() const;

  // 辅助函数:仅用于主动放弃该Client，用户需要明确状态后再使用
  Result<Unit, RichError> setGiveUpSignal();

  //  辅助函数:返回实时时间
  int64_t nowMsImpl();

  signals:
      void dataReady(QModbusDataUnit units); // 或你自定义的轻量结构
      void readFailed(QString  str);
      void writeFailed(QString  str);
      void writeSucceeded(int startAddress,int quantity);
};

// 唯一创建入口。失败返回 Error，不抛异常（含分配失败 -> Internal）。
Result<std::unique_ptr<Client>, RichError>
Client::create(ClientConfig config) noexcept {
     try {
    // new 失败会抛出 std::bad_alloc，由外层的 catch 捕获
    auto client = std::unique_ptr<Client>(
        new Client(config));

    auto configResult = client->m_impl->configureClient(false);
    if (configResult.is_fail()) {
      return Result<std::unique_ptr<Client>, RichError>::error(
          RichError{configResult.get_error()->code(),
                    configResult.get_error()->what()});
    }

    // 这是有意为之的“急切连接”策略，确保对象返回即可用。
    // 若需后台异步恢复，请上层捕获错误后自行重试。
    auto connectResult = client->connect();
    if (connectResult.is_fail()) {
      return Result<std::unique_ptr<Client>, RichError>::error(
          RichError{connectResult.get_error()->code(),
                    connectResult.get_error()->what()});
    }
    return Result<std::unique_ptr<Client>, RichError>::success(
        std::move(client));

  } catch (const std::bad_alloc &e) {
    // 处理内存分配失败
    return Result<std::unique_ptr<Client>, RichError>::error(
        RichError{RichError::ErrorCode::INTERNAL_ERROR,"Memory allocation failed: " + std::string(e.what())});
  } catch (const std::system_error &e) {
    // 处理线程创建等系统级失败
    return Result<std::unique_ptr<Client>, RichError>::error(
        RichError{RichError::ErrorCode::SDK_ERROR,"System error: " + std::string(e.what())});
  } catch (const std::exception &e) {
    // 捕获其他标准异常作为兜底
    return Result<std::unique_ptr<Client>, RichError>::error(
        RichError{RichError::ErrorCode::INTERNAL_ERROR,"Unexpected error: " + std::string(e.what())});
  } catch (...) {
    // 捕获未知异常作为最终保障
    return Result<std::unique_ptr<Client>, RichError>::error(
        RichError{RichError::ErrorCode::UNKNOWN,"Unknown error"});
  } 
}

Client::Client(ClientConfig config) {
  m_recreateSync = std::make_shared<RecreateSync>();
  m_recreateSync->m_sdkForRecreate = std::make_shared<RealSdk>();
  std::shared_ptr<CallbackContext> callbackContext_ =
      std::make_shared<CallbackContext>();
  Client::Impl::QModbus_ClientPtr raw(
      m_recreateSync->m_sdkForRecreate->clientNew(config),
      QModbus_ClientDeleter{m_recreateSync->m_sdkForRecreate,
                            callbackContext_});
  if (raw) {
    m_impl = std::make_shared<Impl>(config, std::move(raw), callbackContext_,
                                    m_recreateSync->m_sdkForRecreate);
  } else {
    throw std::bad_alloc(); // 内存分配失败
  }
}

// Client::Impl
Result<Unit, RichError>
Client::Impl::batchRead(const ReadRequest &reqs) 
{
  auto pImplQuote = shared_from_this();
  if (pImplQuote->getEffectiveStopSignal()) {
    return Result<Unit, RichError>::error(
        RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                  "client is shutting down"});
  }
  InFlightGuard inFlightGuard{pImplQuote->m_taskCvMx, pImplQuote->m_taskCv,
                              pImplQuote->m_taskCounter};


  if (pImplQuote->m_lifeState.load(std::memory_order_acquire) ==
      LifeState::GIVEN_UP) {
    return Result<Unit, RichError>::error(
        RichError{RichError::ErrorCode::NOT_CONNECTED,
                  "batchRead:client life state is GIVEN_UP"});
  }
  if (pImplQuote->m_lifeState.load(std::memory_order_acquire) ==
      LifeState::RECOVERING) {
    return Result<Unit, RichError>::error(
        RichError{RichError::ErrorCode::NOT_CONNECTED,
                  "batchRead: client life state is RECOVERING"});
  }
  if (pImplQuote->m_connState.load(std::memory_order_acquire) !=
      ConnState::Idle) {
    return Result<Unit, RichError>::error(
        RichError{RichError::ErrorCode::THREAD_BUSY,
                  "batchRead:client life state is THREAD_BUSY"});
  }

  bool result = isHealthyCached();
  if (!result) {
    return Result<Unit, RichError>::error(
        RichError{RichError::ErrorCode::NOT_CONNECTED,
                  "batchRead: connection is not healthy"});
  }

  if (!pImplQuote->m_impl) {
    return Result<Unit, RichError>::error(
        RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                  "batchRead: client is NullPtr"});
  } 
  QModbus_ClientPtr &client=pImplQuote->m_impl;

  uint32_t attempts =  1;
  auto startTime = m_sdk->nowMs();
  {
    for (uint32_t retry = 0; retry < attempts; ++retry) {

      if (pImplQuote->getEffectiveStopSignal()) {
        return Result<Unit, RichError>::error(
            RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                      "batchRead: client is shutting down"});
      }

      if (pImplQuote->m_lifeState.load(std::memory_order_acquire) ==
          LifeState::GIVEN_UP) {
        return Result<Unit, RichError>::error(
            RichError{RichError::ErrorCode::NOT_CONNECTED,
                      "batchRead:client life state is GIVEN_UP"});
      }
      if (pImplQuote->m_lifeState.load(std::memory_order_acquire) ==
          LifeState::RECOVERING) {
        return Result<Unit, RichError>::error(
            RichError{RichError::ErrorCode::NOT_CONNECTED,
                      "batchRead:client life state is RECOVERING"});
      }
      if (pImplQuote->m_connState.load(std::memory_order_acquire) !=
          ConnState::Idle) {
        return Result<Unit, RichError>::error(
            RichError{RichError::ErrorCode::THREAD_BUSY,
                      "batchRead:client life state is THREAD_BUSY"});
      }

      QModbusDataUnit::RegisterType type;
      switch (reqs.function) {
      case FunctionCode::ReadHoldingRegisters:
        type = QModbusDataUnit::HoldingRegisters;
        break;
      case FunctionCode::ReadInputRegisters:
        type = QModbusDataUnit::InputRegisters;
        break;
      case FunctionCode::WriteSingleRegister:
      case FunctionCode::WriteMultipleRegisters:
        return Result<Unit, RichError>::error(
            RichError{""}); // 读路径不支持写功能码
      }
      QModbusDataUnit unit(type, reqs.startAddress, reqs.quantity);
      QModbusReply *reply;

      {
        std::lock_guard<std::mutex> sdkLock(m_sdkMutex);
        reply = m_sdk->serviceRead(client.get(), reqs, &unit);
      }

      if (!reply) {
        // 情况 1：同步失败（参数非法 / 客户端未连接 / 忙等）。
        // 此时不会产生任何异步信号，必须在这里立即返回错误。
        // 由于函数返回 QModbusReply*，NullPtr 即为失败信号；
        // *response 未被动过，满足"保持调用前状态"的约定。
        return Result<Unit, RichError>::error(
            RichError{
                RichError::ErrorCode::SERVICE_FAILED,
                "batchRead:client invalid parameter/disconnect/busy wait"});
      }

      // 情况 2 / 3：请求已入队，结果异步到达。
      // 注意：这里必须捕获 response 与 reply，且绝不能在此处 delete reply。
      auto impl_weak = pImplQuote->m_callbackContext->implWeak;
      QObject::connect(
          reply, &QModbusReply::finished, reply, [reply, reqs, impl_weak]() {
            auto impl = impl_weak.lock();
            if (!impl) {
              reply->deleteLater();
              return;
            }

            if (reply->error() != QModbusDevice::NoError) {
              emit impl->readFailed(reply->errorString());
              reply->deleteLater();
              return;
            }

            const QModbusDataUnit result = reply->result();
            if (result.valueCount() != static_cast<qsizetype>(reqs.quantity)) {
              emit impl->readFailed(QStringLiteral("length mismatch"));
              reply->deleteLater();
              return;
            }

            emit impl->dataReady(result);
            reply->deleteLater();
          });
    }
  }

  return Result<Unit, RichError>::error(
      RichError{RichError::ErrorCode::INTERNAL_ERROR,
                "batchRead: unreachable code path"});
}

Result<Unit, RichError>
Client::Impl::batchWrite(const WriteRequest &reqs,
                         const QVector<quint16> &values)
{
  auto pImplQuote = shared_from_this();

  // ---- 与 batchRead 相同的入口守卫 ----
  if (pImplQuote->getEffectiveStopSignal()) {
    return Result<Unit, RichError>::error(
        RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                  "client is shutting down"});
  }
  InFlightGuard inFlightGuard{pImplQuote->m_taskCvMx, pImplQuote->m_taskCv,
                              pImplQuote->m_taskCounter};

  if (pImplQuote->m_lifeState.load(std::memory_order_acquire) ==
      LifeState::GIVEN_UP) {
    return Result<Unit, RichError>::error(
        RichError{RichError::ErrorCode::NOT_CONNECTED,
                  "batchWrite:client life state is GIVEN_UP"});
  }
  if (pImplQuote->m_lifeState.load(std::memory_order_acquire) ==
      LifeState::RECOVERING) {
    return Result<Unit, RichError>::error(
        RichError{RichError::ErrorCode::NOT_CONNECTED,
                  "batchWrite: client life state is RECOVERING"});
  }
  if (pImplQuote->m_connState.load(std::memory_order_acquire) !=
      ConnState::Idle) {
    return Result<Unit, RichError>::error(
        RichError{RichError::ErrorCode::THREAD_BUSY,
                  "batchWrite:client life state is THREAD_BUSY"});
  }

  if (!isHealthyCached()) {
    return Result<Unit, RichError>::error(
        RichError{RichError::ErrorCode::NOT_CONNECTED,
                  "batchWrite: connection is not healthy"});
  }

  if (!pImplQuote->m_impl) {
    return Result<Unit, RichError>::error(
        RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                  "batchWrite: client is NullPtr"});
  }
  QModbus_ClientPtr &client = pImplQuote->m_impl;

  uint32_t attempts = 1;   // 与 batchRead 一致：本地失败不重试
  auto startTime = m_sdk->nowMs();
  Q_UNUSED(startTime);
  {
    for (uint32_t retry = 0; retry < attempts; ++retry) {

      // 每次重试前重新检查停止/生命周期/连接状态（与 batchRead 对称）
      if (pImplQuote->getEffectiveStopSignal()) {
        return Result<Unit, RichError>::error(
            RichError{RichError::ErrorCode::ALREADY_TERMINATED,
                      "batchWrite: client is shutting down"});
      }
      if (pImplQuote->m_lifeState.load(std::memory_order_acquire) ==
          LifeState::GIVEN_UP) {
        return Result<Unit, RichError>::error(
            RichError{RichError::ErrorCode::NOT_CONNECTED,
                      "batchWrite:client life state is GIVEN_UP"});
      }
      if (pImplQuote->m_lifeState.load(std::memory_order_acquire) ==
          LifeState::RECOVERING) {
        return Result<Unit, RichError>::error(
            RichError{RichError::ErrorCode::NOT_CONNECTED,
                      "batchWrite: client life state is RECOVERING"});
      }
      if (pImplQuote->m_connState.load(std::memory_order_acquire) !=
          ConnState::Idle) {
        return Result<Unit, RichError>::error(
            RichError{RichError::ErrorCode::THREAD_BUSY,
                      "batchWrite:client life state is THREAD_BUSY"});
      }

      // 功能码 -> 寄存器类型；写路径只接受写功能码
      QModbusDataUnit::RegisterType type;
      switch (reqs.function) {
      case FunctionCode::WriteSingleRegister:
      case FunctionCode::WriteMultipleRegisters:
        type = QModbusDataUnit::HoldingRegisters;
        break;
      case FunctionCode::ReadHoldingRegisters:
      case FunctionCode::ReadInputRegisters:
        return Result<Unit, RichError>::error(
            RichError{RichError::ErrorCode::SERVICE_FAILED,
                      "batchWrite: write path does not support read function"});
      }

      // 构造写数据单元（注意是 values 构造，不是 quantity）
      QModbusDataUnit unit(type, reqs.startAddress, values);

      QModbusReply *reply;
      {
        std::lock_guard<std::mutex> sdkLock(m_sdkMutex);
        reply = m_sdk->serviceWrite(client.get(), reqs, unit);
      }

      if (!reply) {
        // 同步失败：参数非法 / 未连接 / 忙。重试无意义，直接返回。
        return Result<Unit, RichError>::error(
            RichError{
                RichError::ErrorCode::SERVICE_FAILED,
                "batchWrite:client invalid parameter/disconnect/busy wait"});
      }

      // 异步结果：写操作没有数据可读，成功与否只看 error()
      auto impl_weak = pImplQuote->m_callbackContext->implWeak;
      QObject::connect(
          reply, &QModbusReply::finished, reply,
          [reply, reqs, impl_weak]() {
            auto impl = impl_weak.lock();
            if (!impl) {
              reply->deleteLater();
              return;
            }

            if (reply->error() != QModbusDevice::NoError) {
              // 超时 / 协议异常（从站拒绝写，如非法地址、非法值）/ 连接断开
              emit impl->writeFailed(reply->errorString());
              reply->deleteLater();
              return;
            }

            // 成功：可选地校验回显地址/数量（0x06 会回显寄存器值，
            // 0x10 会回显起始地址与数量）。这里只做基础校验示例。
            const QModbusDataUnit result = reply->result();
            if (result.valueCount() != static_cast<qsizetype>(reqs.quantity)) {
              emit impl->writeFailed(QStringLiteral("write echo length mismatch"));
              reply->deleteLater();
              return;
            }

            emit impl->writeSucceeded(reqs.startAddress, reqs.quantity);
            reply->deleteLater();
          });
    }
  }

  return Result<Unit, RichError>::error(
      RichError{RichError::ErrorCode::INTERNAL_ERROR,
                "batchWrite: unreachable code path"});
}
