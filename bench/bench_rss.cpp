// bench/bench_rss.cpp
//
// Long-run soak: sample resident memory while issuing batch reads, so a 24h
// run produces an RSS curve and a call/error count.
//
//   ./bench_rss --hours 24 --interval 60 --hz 1 > rss_24h.csv
//   ./bench_rss --hours 0.01 --interval 5 --hz 20      # ~36 s smoke run
//   ./bench_rss --endpoint opc.tcp://10.0.0.5:4840 --hours 24
//
// stdout is CSV: elapsed_s,rss_kb,calls,errors  (one row per interval).
#include "bench_common.h"

#include "OPCUAPacking/ua.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Args {
  double hours = 24.0;
  double intervalSec = 60.0;
  double hz = 1.0;
  size_t batch = 16;
  uint16_t port = 4892;
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
    if (k == "--hours") {
      a.hours = std::strtod(next(), nullptr);
    } else if (k == "--interval") {
      a.intervalSec = std::strtod(next(), nullptr);
    } else if (k == "--hz") {
      a.hz = std::strtod(next(), nullptr);
    } else if (k == "--batch") {
      a.batch = std::strtoul(next(), nullptr, 10);
    } else if (k == "--port") {
      a.port = static_cast<uint16_t>(std::strtoul(next(), nullptr, 10));
    } else if (k == "--endpoint") {
      a.endpoint = next();
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", k.c_str());
      std::exit(2);
    }
  }
  if (a.hours <= 0.0)
    a.hours = 0.01;
  if (a.intervalSec <= 0.0)
    a.intervalSec = 1.0;
  if (a.hz <= 0.0)
    a.hz = 1.0;
  if (a.batch == 0)
    a.batch = 1;
  return a;
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
  for (size_t i = 0; i < args.batch; ++i)
    reads.push_back({1, "BenchInt", S7DataType::INT});

  const double totalSec = args.hours * 3600.0;
  const double periodMs = 1000.0 / args.hz;

  std::printf("elapsed_s,rss_kb,calls,errors\n");
  std::fflush(stdout);

  const auto start = bench::Clock::now();
  double nextSample = 0.0;
  unsigned long long calls = 0;
  unsigned long long errors = 0;

  while (true) {
    const double elapsed = bench::msSince(start) / 1000.0;
    if (elapsed >= totalSec)
      break;

    const auto loop0 = bench::Clock::now();
    const auto r = client->batchRead(reads);
    ++calls;
    if (r.is_fail())
      ++errors;

    const double now = bench::msSince(start) / 1000.0;
    if (now >= nextSample) {
      std::printf("%.1f,%ld,%llu,%llu\n", now, bench::residentKb(), calls,
                  errors);
      std::fflush(stdout);
      nextSample += args.intervalSec;
      if (nextSample <= now) // long stall: resynchronise instead of bursting
        nextSample = now + args.intervalSec;
    }

    const double busy = bench::msSince(loop0);
    if (busy < periodMs)
      std::this_thread::sleep_for(
          std::chrono::duration<double, std::milli>(periodMs - busy));
  }

  std::printf("%.1f,%ld,%llu,%llu\n", bench::msSince(start) / 1000.0,
              bench::residentKb(), calls, errors);
  std::fprintf(stderr, "soak finished: hours=%.3f calls=%llu errors=%llu\n",
               args.hours, calls, errors);

  (void)client->shutdown();
  return 0;
}
