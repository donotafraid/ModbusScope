// bench/bench_server.cpp
//
// Standalone open62541 server used as the peer of the long-run RSS soak.
//
//   ./bench_server --port 4892                  # run until SIGINT/SIGTERM
//   ./bench_server --port 4892 --seconds 60     # auto-exit (smoke test)
//
// Why a separate process: bench_rss reads its own /proc/self/statm. When the
// server runs in the same process, the "client RSS" curve also contains the
// server's allocations, so it cannot prove anything about the client. Running
// the server out of process and pointing bench_rss at it with --endpoint makes
// the curve client-only.
//
// Prints exactly one readiness line, then stays quiet:
//   READY port=<port> pid=<pid>
#include "bench_common.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#define BENCH_GETPID _getpid
#else
#include <unistd.h>
#define BENCH_GETPID getpid
#endif

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true, std::memory_order_release); }

struct Args {
  uint16_t port = 4892;
  std::string node = "BenchInt";
  double seconds = 0.0; // 0 = run until signalled
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
    if (k == "--port") {
      a.port = static_cast<uint16_t>(std::strtoul(next(), nullptr, 10));
    } else if (k == "--node") {
      const char *v = next();
      a.node = v ? v : a.node;
    } else if (k == "--seconds") {
      a.seconds = std::strtod(next(), nullptr);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", k.c_str());
      std::exit(2);
    }
  }
  return a;
}

} // namespace

int main(int argc, char **argv) {
  bench::initLogging();
  const Args args = parse(argc, argv);

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  bench::Server server(args.port, args.node);

  std::printf("READY port=%u pid=%ld\n", args.port,
              static_cast<long>(BENCH_GETPID()));
  std::fflush(stdout);

  const auto start = bench::Clock::now();
  while (!g_stop.load(std::memory_order_acquire)) {
    if (args.seconds > 0.0 && bench::msSince(start) >= args.seconds * 1000.0)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  return 0;
}
