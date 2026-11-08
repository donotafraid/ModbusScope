// bench/bench_latency.cpp
//
// 8-thread (configurable) shared-client batchRead/batchWrite latency benchmark.
//
//   ./bench_latency --threads 8 --ops 8000 --batch 16
//   ./bench_latency --endpoint opc.tcp://127.0.0.1:4840 --write
//   ./bench_latency --endpoint ... --rate 2688 --per-thread-client
//
// Prints one machine-readable summary line and one latency line (P50/P90/P99/
// P99.9/max/mean, milliseconds) to stdout. Exit status is non-zero if any call
// failed.
#include "bench_common.h"

#include "OPCUAPacking/ua.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Args {
  size_t threads = 8;
  size_t ops = 8000;
  size_t batch = 16;
  size_t repeats = 1;
  uint16_t port = 4890;
  std::string endpoint;
  bool write = false;
  double rate = 0.0;             // 0 = unlimited; else global calls/s
  bool perThreadClient = false;  // one OPC_UA_Client per worker thread
  uint32_t logLevel = ClientConfig::LOG_ERROR; // 100=TRACE .. 600=FATAL
};

Args parse(int argc, char **argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    if (k == "--help" || k == "-h") {
      std::printf(
          "usage: bench_latency [--threads N] [--ops N] [--batch N]\n"
          "                     [--repeats N] [--port N] [--endpoint URL]\n"
          "                     [--write] [--rate R] [--per-thread-client]\n"
          "\n"
          "  --rate R             global target rate (calls/s); 0 = unlimited\n"
          "  --per-thread-client  one OPC_UA_Client per worker thread\n"
          "  --log-level N        SDK log threshold 100..600 (default 500)\n");
      std::exit(0);
    }
    auto next = [&]() -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", k.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (k == "--threads") {
      a.threads = std::strtoul(next(), nullptr, 10);
    } else if (k == "--ops") {
      a.ops = std::strtoul(next(), nullptr, 10);
    } else if (k == "--batch") {
      a.batch = std::strtoul(next(), nullptr, 10);
    } else if (k == "--port") {
      a.port = static_cast<uint16_t>(std::strtoul(next(), nullptr, 10));
    } else if (k == "--endpoint") {
      a.endpoint = next();
    } else if (k == "--repeats") {
      a.repeats = std::strtoul(next(), nullptr, 10);
    } else if (k == "--write") {
      a.write = true;
    } else if (k == "--rate") {
      a.rate = std::strtod(next(), nullptr);
      if (a.rate < 0) a.rate = 0;
    } else if (k == "--per-thread-client") {
      a.perThreadClient = true;
    } else if (k == "--log-level") {
      a.logLevel = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", k.c_str());
      std::exit(2);
    }
  }
  if (a.threads == 0) a.threads = 1;
  if (a.ops == 0) a.ops = 1;
  if (a.batch == 0) a.batch = 1;
  if (a.repeats == 0) a.repeats = 1;
  return a;
}

} // namespace

int main(int argc, char **argv) {
  bench::initLogging();
  const Args args = parse(argc, argv);

  std::unique_ptr<bench::Server> server;
  std::string url = args.endpoint;
  const char *nodeName = "BenchInt";
  if (url.empty()) {
    server = std::make_unique<bench::Server>(args.port, nodeName);
    url = "opc.tcp://127.0.0.1:" + std::to_string(args.port);
  }

  ClientConfig cfg;
  cfg.applyProfile(ClientConfig::PROFILE_LOCAL);
  cfg.logLevel = args.logLevel; // 500 keeps the measured stream quiet
  cfg.watchdogIntervalMs = 1000;
  cfg.giveUpThresholdMs = 60000;

  auto created = OPC_UA_Client::create(url, cfg, /*useDefault=*/false);
  if (created.is_fail()) {
    std::fprintf(stderr, "create failed: %s\n",
                 created.get_error()->codeName());
    return 2;
  }
  auto client = std::move(*created.get());

  std::vector<OPC_UA_Client::ReadValue> reads;
  std::vector<OPC_UA_Client::WriteValue> writes;
  for (size_t i = 0; i < args.batch; ++i) {
    reads.push_back({1, nodeName, S7DataType::INT});
    writes.push_back({{1, nodeName, S7DataType::INT}, static_cast<int16_t>(i)});
  }

  struct Run {
    size_t calls = 0;
    double wallMs = 0.0;
    double callsPerSec = 0.0;
    double p50 = 0.0, p90 = 0.0, p99 = 0.0, p999 = 0.0, max = 0.0, mean = 0.0;
    uint64_t errors = 0;
  };

  const size_t perThread = args.ops / args.threads;
  std::vector<Run> runs;
  runs.reserve(args.repeats);
  uint64_t totalErrors = 0;
  std::atomic<uint64_t> setup_errors{0};

  // --rate is a global target; split it across all worker threads. A worker's
  // schedule is open-loop: call i is planned at rateStart + (i+1)*interval, and
  // the latency is measured from that planned instant. When the system keeps
  // up, the sleep burns the gap; when it falls behind, the measured tail grows
  // instead of being hidden by a closed-loop "send when ready" model.
  const double perThreadRate =
      (args.rate > 0 && args.threads > 0)
          ? args.rate / static_cast<double>(args.threads)
          : 0.0;
  const int64_t intervalNs =
      perThreadRate > 0 ? static_cast<int64_t>(1e9 / perThreadRate) : 0;

  for (size_t rep = 0; rep < args.repeats; ++rep) {
    // Shared-client mode warms up the shared instance once per repeat.
    // Per-thread mode warms up inside each worker after its own client is
    // created, so the first measured call has already negotiated a session.
    if (!args.perThreadClient) {
      for (int i = 0; i < 3; ++i) {
        if (args.write)
          (void)client->batchWrite(writes);
        else
          (void)client->batchRead(reads);
      }
    }

    std::vector<std::vector<double>> latencies(args.threads);
    std::atomic<uint64_t> errors{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> pool;
    pool.reserve(args.threads);

    for (size_t t = 0; t < args.threads; ++t) {
      pool.emplace_back([&, t] {
        OPC_UA_Client *cli = client.get();
        std::unique_ptr<OPC_UA_Client> own;
        if (args.perThreadClient) {
          auto c = OPC_UA_Client::create(url, cfg, /*useDefault=*/false);
          if (c.is_fail()) {
            setup_errors.fetch_add(1, std::memory_order_relaxed);
            return;
          }
          own = std::move(*c.get());
          cli = own.get();
          for (int w = 0; w < 3; ++w) {
            if (args.write)
              (void)cli->batchWrite(writes);
            else
              (void)cli->batchRead(reads);
          }
        }

        latencies[t].reserve(perThread);
        while (!go.load(std::memory_order_acquire))
          std::this_thread::yield();

        const auto rateStart = bench::Clock::now();
        for (size_t i = 0; i < perThread; ++i) {
          auto t0 = bench::Clock::now();
          if (perThreadRate > 0) {
            const auto target =
                rateStart +
                std::chrono::nanoseconds(intervalNs *
                                         static_cast<int64_t>(i + 1));
            const auto now = bench::Clock::now();
            if (now < target)
              std::this_thread::sleep_for(target - now);
            t0 = target;  // measure from the planned instant
          }
          const bool ok = args.write ? cli->batchWrite(writes).is_success()
                                     : cli->batchRead(reads).is_success();
          latencies[t].push_back(bench::msSince(t0));
          if (!ok)
            errors.fetch_add(1, std::memory_order_relaxed);
        }

        if (own)
          (void)own->shutdown();
      });
    }

    const auto wall0 = bench::Clock::now();
    go.store(true, std::memory_order_release);
    for (auto &th : pool)
      th.join();
    const double wallMs = bench::msSince(wall0);

    if (setup_errors.load() > 0) {
      std::fprintf(stderr,
                   "ERROR: %llu thread(s) failed to create per-thread client\n",
                   static_cast<unsigned long long>(setup_errors.load()));
      return 2;
    }

    std::vector<double> all;
    for (const auto &v : latencies)
      all.insert(all.end(), v.begin(), v.end());
    all = bench::sorted(std::move(all));

    Run run;
    run.calls = all.size();
    run.wallMs = wallMs;
    run.callsPerSec =
        wallMs > 0.0 ? static_cast<double>(run.calls) * 1000.0 / wallMs : 0.0;
    run.p50 = bench::percentile(all, 50.0);
    run.p90 = bench::percentile(all, 90.0);
    run.p99 = bench::percentile(all, 99.0);
    run.p999 = bench::percentile(all, 99.9);
    run.max = all.empty() ? 0.0 : all.back();
    run.mean = bench::mean(all);
    run.errors = errors.load();
    totalErrors += run.errors;

    std::printf("run=%zu/%zu mode=%s threads=%zu ops=%zu batch=%zu calls=%zu "
                "wall_ms=%.1f calls_s=%.1f items_s=%.1f errors=%llu\n",
                rep + 1, args.repeats, args.write ? "write" : "read",
                args.threads, args.ops, args.batch, run.calls, run.wallMs,
                run.callsPerSec,
                run.callsPerSec * static_cast<double>(args.batch),
                static_cast<unsigned long long>(run.errors));
    std::printf("run=%zu latency_ms p50=%.3f p90=%.3f p99=%.3f p999=%.3f "
                "max=%.3f mean=%.3f\n",
                rep + 1, run.p50, run.p90, run.p99, run.p999, run.max, run.mean);
    runs.push_back(run);
  }

  // A single run's tail is not reproducible: host scheduling moves p99 by more
  // than an order of magnitude between runs. Print the median across runs so a
  // report can quote a stable number instead of one lucky/unlucky sample.
  if (runs.size() > 1) {
    auto medianOf = [&](double (*pick)(const Run &)) {
      std::vector<double> v;
      v.reserve(runs.size());
      for (const Run &r : runs)
        v.push_back(pick(r));
      v = bench::sorted(std::move(v));
      return bench::percentile(v, 50.0);
    };
    std::printf("median mode=%s threads=%zu ops=%zu batch=%zu repeats=%zu "
                "calls_s=%.1f p50=%.3f p90=%.3f p99=%.3f p999=%.3f max=%.3f "
                "mean=%.3f errors=%llu\n",
                args.write ? "write" : "read", args.threads, args.ops, args.batch,
                runs.size(),
                medianOf([](const Run &r) { return r.callsPerSec; }),
                medianOf([](const Run &r) { return r.p50; }),
                medianOf([](const Run &r) { return r.p90; }),
                medianOf([](const Run &r) { return r.p99; }),
                medianOf([](const Run &r) { return r.p999; }),
                medianOf([](const Run &r) { return r.max; }),
                medianOf([](const Run &r) { return r.mean; }),
                static_cast<unsigned long long>(totalErrors));
  }

  if (!args.perThreadClient)
    (void)client->shutdown();
  return totalErrors == 0 ? 0 : 1;
}