// bench/bench_common.h
//
// Shared helpers for the benchmark executables. This header is internal to the
// benchmark programs and is never installed or included by the library.
//
// The benchmark programs run against either an endpoint passed with --endpoint
// or an in-process open62541 server they start themselves, so a single command
// reproduces a measurement on a developer machine.
#ifndef OPCUAPACKING_BENCH_COMMON_H
#define OPCUAPACKING_BENCH_COMMON_H

#include <open62541/common.h>
#include <open62541/nodeids.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <iostream>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#else
#include <sys/resource.h>
#endif

namespace bench {

// The library logs through spdlog's default logger (stdout). A benchmark's
// stdout is a machine-readable stream (bench_rss writes CSV there), so route
// the library log to stderr before any client is created.
inline void initLogging() {
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(std::cerr);
  spdlog::set_default_logger(
      std::make_shared<spdlog::logger>("bench", std::move(sink)));
}

// open62541's default server logger writes to stdout, which is the benchmark's
// machine-readable result stream. Keep the logger object (and its clear
// callback) but redirect only the log function to stderr. The SDK is built
// with UA_LOGLEVEL=100, so filter here as well: a 2k-op smoke run otherwise
// wrote ~5 MB of trace frames.
inline std::atomic<uint32_t> benchServerLogLevel{UA_LOGLEVEL_WARNING};

inline void benchLog(void * /*context*/, UA_LogLevel level, UA_LogCategory,
                     const char *msg, va_list args) {
  if (static_cast<uint32_t>(level) <
      benchServerLogLevel.load(std::memory_order_relaxed)) {
    return;
  }
  if (!msg) {
    return;
  }
  std::vfprintf(stderr, msg, args);
  std::fputc('\n', stderr);
}

// Static logger with clear == nullptr: used as a fallback only.
inline UA_Logger benchLogger{benchLog, nullptr, nullptr};

using Clock = std::chrono::steady_clock;

inline double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Linear-interpolation percentile over a sorted vector.
inline double percentile(const std::vector<double> &sorted, double p) {
  if (sorted.empty())
    return 0.0;
  if (sorted.size() == 1)
    return sorted.front();
  const double idx = (p / 100.0) * static_cast<double>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(idx);
  const size_t hi = std::min(lo + 1, sorted.size() - 1);
  const double frac = idx - static_cast<double>(lo);
  return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

inline std::vector<double> sorted(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v;
}

inline double mean(const std::vector<double> &v) {
  if (v.empty())
    return 0.0;
  double s = 0.0;
  for (double x : v)
    s += x;
  return s / static_cast<double>(v.size());
}

// Resident set size in KiB. Linux reads /proc/self/statm; other platforms fall
// back to getrusage (Linux reports KiB, macOS reports bytes).
inline long residentKb() {
#if defined(__linux__)
  std::FILE *f = std::fopen("/proc/self/statm", "r");
  if (!f)
    return -1;
  long sizePages = 0;
  long residentPages = 0;
  const int n = std::fscanf(f, "%ld %ld", &sizePages, &residentPages);
  std::fclose(f);
  if (n != 2)
    return -1;
  const long pageKb = static_cast<long>(sysconf(_SC_PAGESIZE) / 1024);
  return residentPages * pageKb;
#else
  struct rusage ru;
  std::memset(&ru, 0, sizeof(ru));
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;
#endif
}

// In-process OPC UA server exposing one read/write INT16 node at ns=1.
// startServing()/stopServing() can be repeated on the same server object,
// which is what the recovery benchmark uses to simulate a link outage.
class Server {
public:
  explicit Server(uint16_t port, std::string nodeName = "BenchInt")
      : port_(port), nodeName_(std::move(nodeName)) {
    UA_ServerConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.logging = &benchLogger;
    if (UA_ServerConfig_setDefault(&cfg) != UA_STATUSCODE_GOOD) {
      std::fprintf(stderr, "UA_ServerConfig_setDefault failed\n");
      std::exit(2);
    }
    // UA_Server_newWithConfig moves cfg (shallow copy) into the server.
    server_ = UA_Server_newWithConfig(&cfg);
    if (!server_) {
      UA_ServerConfig_clean(&cfg);
      std::fprintf(stderr, "UA_Server_newWithConfig failed\n");
      std::exit(2);
    }
    UA_ServerConfig *config = UA_Server_getConfig(server_);
    if (config && config->logging) {
      // The global level is used because some SDK code paths keep a second
      // logger pointer (PKI/session); a per-object context would not reach all
      // of them.
      config->logging->log = benchLog;
    }
    configurePort(port_);
    addInt16Node();
    startServing();
  }

  ~Server() {
    stopServing();
    if (server_)
      UA_Server_delete(server_);
  }

  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;

  void stopServing() {
    if (!started_)
      return;
    running_.store(false, std::memory_order_release);
    if (thread_.joinable())
      thread_.join();
    UA_Server_run_shutdown(server_);
    started_ = false;
  }

  void startServing() {
    if (started_)
      return;
    if (UA_Server_run_startup(server_) != UA_STATUSCODE_GOOD) {
      std::fprintf(stderr, "UA_Server_run_startup failed on port %u\n", port_);
      std::exit(2);
    }
    started_ = true;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] {
      // waitInternal=true lets the event loop block until the next network event
      // or cyclic callback. The previous waitInternal=false busy-spun a full
      // CPU core, which on a small CI machine competes with the client threads
      // and inflates the measured tail latency. The server's 1 s housekeeping
      // callback bounds the block, so stopServing()'s join returns promptly.
      while (running_.load(std::memory_order_acquire))
        UA_Server_run_iterate(server_, true);
    });
  }

  void restart() {
    stopServing();
    startServing();
  }

  uint16_t port() const { return port_; }
  const std::string &nodeName() const { return nodeName_; }

private:
  void configurePort(uint16_t port) {
    UA_ServerConfig *config = UA_Server_getConfig(server_);
    if (config->serverUrls && config->serverUrlsSize > 0)
      UA_Array_delete(config->serverUrls, config->serverUrlsSize,
                      &UA_TYPES[UA_TYPES_STRING]);
    config->serverUrls = nullptr;
    config->serverUrlsSize = 0;

    std::string urlStr = "opc.tcp://127.0.0.1:" + std::to_string(port);
    UA_String url = UA_STRING_ALLOC(urlStr.c_str());
    const UA_StatusCode rc = UA_Array_copy(
        &url, 1, reinterpret_cast<void **>(&config->serverUrls),
        &UA_TYPES[UA_TYPES_STRING]);
    UA_String_clear(&url);
    if (rc != UA_STATUSCODE_GOOD) {
      std::fprintf(stderr, "failed to set server URLs\n");
      std::exit(2);
    }
    config->serverUrlsSize = 1;
  }

  void addInt16Node() {
    UA_Int16 value = 0;
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Variant_setScalar(&attr.value, &value, &UA_TYPES[UA_TYPES_INT16]);
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

    const int nameSpace = 1;
    UA_NodeId nodeId = UA_NODEID_STRING(nameSpace, nodeName_.data());
    UA_QualifiedName name =
        UA_QUALIFIEDNAME(nameSpace, const_cast<char *>(nodeName_.c_str()));
    UA_NodeId parent = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId parentRef = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    UA_Server_addVariableNode(server_, nodeId, parent, parentRef, name,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                              attr, nullptr, nullptr);
  }

  UA_Server *server_ = nullptr;
  uint16_t port_;
  std::string nodeName_;
  std::atomic<bool> running_{false};
  bool started_ = false;
  std::thread thread_;
};

} // namespace bench

#endif // OPCUAPACKING_BENCH_COMMON_H
