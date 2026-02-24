#include "example/common/ExampleConfig.h"
#include "galay-redis/async/RedisClient.h"
#include <galay-kernel/kernel/Runtime.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace galay::kernel;
using namespace galay::redis;

namespace {

struct BenchmarkOptions {
    std::string host = galay::redis::example::kDefaultRedisHost;
    int port = galay::redis::example::kDefaultRedisPort;
    int clients = 10;
    int operations = 100;
    std::string mode = "normal";
    int batch_size = 100;
    bool verbose = true;
};

std::atomic<std::int64_t> g_success{0};
std::atomic<std::int64_t> g_error{0};
std::atomic<std::int64_t> g_timeout{0};
std::atomic<int> g_completed_clients{0};
std::mutex g_completed_mutex;
std::condition_variable g_completed_cv;

bool parseInt(const std::string& text, int& value)
{
    try {
        size_t used = 0;
        const int parsed = std::stoi(text, &used);
        if (used != text.size()) return false;
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

void printUsage(const char* program)
{
    std::cout << "Usage: " << program
              << " [-h host] [-p port] [-c clients] [-n operations] "
                 "[-m normal|pipeline] [-b batch_size] [-q]"
              << std::endl;
}

bool parseArgs(int argc, char* argv[], BenchmarkOptions& options, bool& show_help)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            show_help = true;
            return false;
        }
        if (arg == "-q" || arg == "--quiet") {
            options.verbose = false;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for argument: " << arg << std::endl;
            return false;
        }

        const std::string value = argv[++i];
        if (arg == "-h" || arg == "--host") {
            options.host = value;
            continue;
        }
        if (arg == "-p" || arg == "--port") {
            if (!parseInt(value, options.port) || options.port <= 0 || options.port > 65535) {
                std::cerr << "Invalid port: " << value << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "-c" || arg == "--clients") {
            if (!parseInt(value, options.clients) || options.clients <= 0) {
                std::cerr << "Invalid clients: " << value << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "-n" || arg == "--operations") {
            if (!parseInt(value, options.operations) || options.operations <= 0) {
                std::cerr << "Invalid operations: " << value << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "-m" || arg == "--mode") {
            options.mode = value;
            continue;
        }
        if (arg == "-b" || arg == "--batch-size") {
            if (!parseInt(value, options.batch_size) || options.batch_size <= 0) {
                std::cerr << "Invalid batch-size: " << value << std::endl;
                return false;
            }
            continue;
        }

        std::cerr << "Unknown argument: " << arg << std::endl;
        return false;
    }

    if (options.mode != "normal" && options.mode != "pipeline") {
        std::cerr << "Invalid mode: " << options.mode << ", expected normal|pipeline" << std::endl;
        return false;
    }
    if (options.mode == "pipeline" && options.batch_size <= 0) {
        std::cerr << "batch-size must be > 0 in pipeline mode" << std::endl;
        return false;
    }
    return true;
}

void markClientCompleted()
{
    g_completed_clients.fetch_add(1, std::memory_order_relaxed);
    g_completed_cv.notify_one();
}

template <typename T>
void countSingleResult(const T& result, std::int64_t& success, std::int64_t& error, std::int64_t& timeout)
{
    if (result && result.value()) {
        ++success;
        return;
    }

    if (!result) {
        if (result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
            ++timeout;
        } else {
            ++error;
        }
        return;
    }

    ++error;
}

template <typename T>
void countBatchResult(
    const T& result,
    std::int64_t count,
    std::int64_t& success,
    std::int64_t& error,
    std::int64_t& timeout)
{
    if (result && result.value()) {
        success += count;
        return;
    }

    if (!result) {
        if (result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
            timeout += count;
        } else {
            error += count;
        }
        return;
    }

    error += count;
}

Coroutine benchmarkNormal(IOScheduler* scheduler, const BenchmarkOptions* options, int client_id)
{
    RedisClient client(scheduler);
    std::int64_t local_success = 0;
    std::int64_t local_error = 0;
    std::int64_t local_timeout = 0;

    auto connect_result = co_await client.connect(options->host, options->port).timeout(std::chrono::seconds(5));
    if (!connect_result) {
        local_error += static_cast<std::int64_t>(options->operations) * 2;
        g_success.fetch_add(local_success, std::memory_order_relaxed);
        g_error.fetch_add(local_error, std::memory_order_relaxed);
        g_timeout.fetch_add(local_timeout, std::memory_order_relaxed);
        markClientCompleted();
        co_return;
    }

    const auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < options->operations; ++i) {
        const std::string key = "bench:normal:" + std::to_string(client_id) + ":" + std::to_string(i);
        const std::string value = "value_" + std::to_string(i);

        auto set_result = co_await client.set(key, value).timeout(std::chrono::seconds(5));
        countSingleResult(set_result, local_success, local_error, local_timeout);

        auto get_result = co_await client.get(key).timeout(std::chrono::seconds(5));
        countSingleResult(get_result, local_success, local_error, local_timeout);
    }
    const auto end = std::chrono::high_resolution_clock::now();

    (void)co_await client.close();

    g_success.fetch_add(local_success, std::memory_order_relaxed);
    g_error.fetch_add(local_error, std::memory_order_relaxed);
    g_timeout.fetch_add(local_timeout, std::memory_order_relaxed);

    if (options->verbose) {
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Client " << client_id << " finished normal mode in "
                  << duration.count() << "ms" << std::endl;
    }

    markClientCompleted();
}

Coroutine benchmarkPipeline(IOScheduler* scheduler, const BenchmarkOptions* options, int client_id)
{
    RedisClient client(scheduler);
    std::int64_t local_success = 0;
    std::int64_t local_error = 0;
    std::int64_t local_timeout = 0;

    auto connect_result = co_await client.connect(options->host, options->port).timeout(std::chrono::seconds(5));
    if (!connect_result) {
        local_error += options->operations;
        g_success.fetch_add(local_success, std::memory_order_relaxed);
        g_error.fetch_add(local_error, std::memory_order_relaxed);
        g_timeout.fetch_add(local_timeout, std::memory_order_relaxed);
        markClientCompleted();
        co_return;
    }

    const auto start = std::chrono::high_resolution_clock::now();
    int offset = 0;
    while (offset < options->operations) {
        const int current_batch = std::min(options->batch_size, options->operations - offset);
        std::vector<std::vector<std::string>> commands;
        commands.reserve(static_cast<size_t>(current_batch));
        for (int i = 0; i < current_batch; ++i) {
            const std::string key = "bench:pipeline:" + std::to_string(client_id) + ":" + std::to_string(offset + i);
            const std::string value = "value_" + std::to_string(offset + i);
            commands.push_back({"SET", key, value});
        }

        auto pipeline_result = co_await client.pipeline(commands).timeout(std::chrono::seconds(5));
        countBatchResult(
            pipeline_result,
            static_cast<std::int64_t>(current_batch),
            local_success,
            local_error,
            local_timeout);
        offset += current_batch;
    }
    const auto end = std::chrono::high_resolution_clock::now();

    (void)co_await client.close();

    g_success.fetch_add(local_success, std::memory_order_relaxed);
    g_error.fetch_add(local_error, std::memory_order_relaxed);
    g_timeout.fetch_add(local_timeout, std::memory_order_relaxed);

    if (options->verbose) {
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Client " << client_id << " finished pipeline mode in "
                  << duration.count() << "ms" << std::endl;
    }

    markClientCompleted();
}

}  // namespace

int main(int argc, char* argv[])
{
    BenchmarkOptions options;
    bool show_help = false;
    if (!parseArgs(argc, argv, options, show_help)) {
        printUsage(argv[0]);
        return show_help ? 0 : 1;
    }

    g_success.store(0, std::memory_order_relaxed);
    g_error.store(0, std::memory_order_relaxed);
    g_timeout.store(0, std::memory_order_relaxed);
    g_completed_clients.store(0, std::memory_order_relaxed);

    std::cout << "==================================================" << std::endl;
    std::cout << "Redis Client Benchmark (B1)" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "Host: " << options.host << ":" << options.port << std::endl;
    std::cout << "Clients: " << options.clients << std::endl;
    std::cout << "Operations per client: " << options.operations << std::endl;
    std::cout << "Mode: " << options.mode << std::endl;
    if (options.mode == "pipeline") {
        std::cout << "Batch size: " << options.batch_size << std::endl;
    }
    const std::int64_t planned_ops =
        options.mode == "pipeline"
            ? static_cast<std::int64_t>(options.clients) * options.operations
            : static_cast<std::int64_t>(options.clients) * options.operations * 2;
    std::cout << "Planned operations: " << planned_ops << std::endl;
    std::cout << "==================================================" << std::endl;

    Runtime runtime;
    runtime.start();

    const auto bench_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < options.clients; ++i) {
        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "Failed to get IO scheduler for client " << i << std::endl;
            runtime.stop();
            return 1;
        }
        if (options.mode == "pipeline") {
            scheduler->spawn(benchmarkPipeline(scheduler, &options, i));
        } else {
            scheduler->spawn(benchmarkNormal(scheduler, &options, i));
        }
    }

    std::unique_lock<std::mutex> lock(g_completed_mutex);
    const bool finished = g_completed_cv.wait_for(lock, std::chrono::seconds(180), [&]() {
        return g_completed_clients.load(std::memory_order_relaxed) >= options.clients;
    });

    const auto bench_end = std::chrono::high_resolution_clock::now();
    runtime.stop();

    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(bench_end - bench_start).count();
    const std::int64_t success = g_success.load(std::memory_order_relaxed);
    const std::int64_t error = g_error.load(std::memory_order_relaxed);
    const std::int64_t timeout = g_timeout.load(std::memory_order_relaxed);
    const std::int64_t total = success + error + timeout;

    std::cout << "\n==================================================" << std::endl;
    std::cout << "Benchmark Results" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "Finished: " << (finished ? "yes" : "no (timeout)") << std::endl;
    std::cout << "Duration: " << duration_ms << "ms" << std::endl;
    std::cout << "Success: " << success << std::endl;
    std::cout << "Error: " << error << std::endl;
    std::cout << "Timeout: " << timeout << std::endl;
    if (duration_ms > 0) {
        const double qps = static_cast<double>(success) / (static_cast<double>(duration_ms) / 1000.0);
        std::cout << "Ops/sec: " << static_cast<std::int64_t>(qps) << std::endl;
    }
    if (total > 0) {
        const double success_rate = static_cast<double>(success) * 100.0 / static_cast<double>(total);
        std::cout << "Success rate: " << success_rate << "%" << std::endl;
    }
    std::cout << "==================================================" << std::endl;

    return finished ? 0 : 2;
}
