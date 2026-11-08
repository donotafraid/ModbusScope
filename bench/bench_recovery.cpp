// bench/bench_recovery.cpp
//
// Disconnect/recovery latency distribution.
//
//   ./bench_recovery --api-cycles 30 --restart-cycles 20
//   ./bench_recovery --endpoint opc.tcp://127.0.0.1:4840 --api-cycles 50
//
// Two measurements:
//   api_reconnect_ms      explicit disconnect() + connect() round trip
//   restart_recovery_ms   server outage -> watchdog-driven healthy again
//                         (only when the benchmark starts its own server)
//
// Prints P50/P90/P99/max for each distribution on stdout.
#include "bench_common.h"

#include "OPCUAPacking/ua.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Args {
  size_t apiCycles = 30;
  size_t restartCycles = 20;
  uint16_t port = 4891;
  std::string endpoint;
};

Args parse(int argc, char **argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", k.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (k == "--api-cycles") {
      a.apiCycles = std::strtoul(next(), nullptr, 10);
    } else if (k == "--restart-cycles") {
      a.restartCycles = std::strtoul(next(), nullptr, 10);
    } else if (k == "--port") {
      a.port = static_cast<uint16_t>(std::strtoul(next(), nullptr, 10));
    } else if (k == "--endpoint") {
      a.endpoint = next();
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", k.c_str());
      std::exit(2);
    }
  }
  return a;
}

void printDistribution(const char *name, std::vector<double> samples) {
  if (samples.empty()) {
    std::printf("%s n=0\n", name);
    return;
  }
  samples = bench::sorted(std::move(samples));
  std::printf("%s n=%zu p50=%.1fms p90=%.1fms p99=%.1fms max=%.1fms "
              "mean=%.1fms\n",
              name, samples.size(), bench::percentile(samples, 50.0),
              bench::percentile(samples, 90.0),
              bench::percentile(samples, 99.0), samples.back(),
              bench::mean(samples));
}

bool waitHealthy(OPC_UA_Client &client, double timeoutMs) {
  const auto t0 = bench::Clock::now();
  while (bench::msSince(t0) < timeoutMs) {
    if (client.isHealthy().is_success())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return client.isHealthy().is_success();
}

} // namespace

int main(int argc, char **argv) {
  bench::initLogging();
  const Args args = parse(argc, argv);

  std::unique_ptr<bench::Server> server;
  std::string url = args.endpoint;
  if (url.empty()) {
    server = std::make_unique<bench::Server>(args.port);
    url = "opc.tcp://127.0.0.1:" + std::to_string(args.port);
  }

  ClientConfig cfg;
  cfg.applyProfile(ClientConfig::PROFILE_LOCAL);
  cfg.logLevel = ClientConfig::LOG_ERROR; // keep the measured stream quiet
  cfg.watchdogIntervalMs = 1000; // fastest the config validation allows
  cfg.giveUpThresholdMs = 60000;

  auto created = OPC_UA_Client::create(url, cfg, /*useDefault=*/false);
  if (created.is_fail()) {
    std::fprintf(stderr, "create failed: %s\n",
                 created.get_error()->codeName());
    return 2;
  }
  auto client = std::move(*created.get());

  // ---- 1) explicit disconnect() + connect() round trip ----
  std::vector<double> apiSamples;
  apiSamples.reserve(args.apiCycles);
  for (size_t i = 0; i < args.apiCycles; ++i) {
    const auto t0 = bench::Clock::now();
    const auto d = client->disconnect();
    const auto c = client->connect();
    const double ms = bench::msSince(t0);
    if (d.is_fail() || c.is_fail()) {
      std::fprintf(stderr, "api cycle %zu failed: d=%s c=%s\n", i,
                   d.is_fail() ? d.get_error()->codeName() : "ok",
                   c.is_fail() ? c.get_error()->codeName() : "ok");
      continue;
    }
    apiSamples.push_back(ms);
  }
  printDistribution("api_reconnect_ms", std::move(apiSamples));

  // ---- 2) server outage -> watchdog recovery ----
  if (server) {
    std::vector<double> recoverySamples;
    recoverySamples.reserve(args.restartCycles);
    for (size_t i = 0; i < args.restartCycles; ++i) {
      server->stopServing();

      // Wait until the client observed the outage; the watchdog drives this.
      const auto down0 = bench::Clock::now();
      while (bench::msSince(down0) < 30000.0 &&
             client->isHealthy().is_success()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }

      server->startServing();
      const auto up0 = bench::Clock::now();
      const bool recovered = waitHealthy(*client, 60000.0);
      const double ms = bench::msSince(up0);
      if (!recovered) {
        std::fprintf(stderr, "restart cycle %zu did not recover\n", i);
        continue;
      }
      recoverySamples.push_back(ms);
    }
    printDistribution("restart_recovery_ms", std::move(recoverySamples));
  } else {
    std::fprintf(stderr,
                 "no --endpoint server owned by this process; restart sweep "
                 "skipped\n");
  }

  (void)client->shutdown();
  return 0;
}
