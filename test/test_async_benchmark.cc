#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <galay/kernel/async/AsyncFactory.h>
#include <galay/kernel/runtime/Runtime.h>
#include <galay/kernel/coroutine/CoSchedulerHandle.hpp>
#include <galay/kernel/coroutine/AsyncWaiter.hpp>
#include "async/AsyncRedisSession.h"

using namespace galay;
using namespace galay::redis;

// 全局统计变量
std::atomic<uint64_t> g_total_requests{0};
std::atomic<uint64_t> g_success_requests{0};
std::atomic<uint64_t> g_failed_requests{0};
std::atomic<bool> g_running{true};

// 压测配置
struct BenchmarkConfig {
    std::string redis_url = "redis://:galay123@140.143.142.251:6379";
    int num_threads = 4;                // 线程数
    int num_sessions_per_thread = 10;   // 每个线程的会话数
    int requests_per_session = 100;     // 每个会话的请求数
    int duration_seconds = 30;          // 压测持续时间（秒）
    bool use_duration = false;          // 是否使用持续时间模式
    bool quiet = false;                 // 安静模式，不输出连接日志
};

// 单个会话的压测协程
Coroutine<nil> benchmarkSession(CoSchedulerHandle handle, const BenchmarkConfig& config, 
                                 int thread_id, int session_id, std::atomic<int>& active_sessions) {
    // 创建异步Redis会话
    AsyncRedisSession session(handle);
    
    // 连接到Redis服务器
    auto connect_result = co_await session.connect(config.redis_url);
    if (!connect_result) {
        std::cerr << "[Thread-" << thread_id << "][Session-" << session_id 
                  << "] Failed to connect: " << connect_result.error().message() << std::endl;
        active_sessions--;
        co_return nil();
    }
    
    int request_count = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    // 执行压测
    while (true) {
        // 检查是否应该停止
        if (config.use_duration) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            if (elapsed >= config.duration_seconds || !g_running) {
                break;
            }
        } else {
            if (request_count >= config.requests_per_session) {
                break;
            }
        }
        
        // 生成唯一的key
        std::string key = "bench_t" + std::to_string(thread_id) + 
                         "_s" + std::to_string(session_id) + 
                         "_r" + std::to_string(request_count);
        std::string value = "value_" + std::to_string(request_count);
        
        // SET操作
        g_total_requests++;
        auto set_result = co_await session.set(key, value);
        if (!set_result || set_result->empty()) {
            g_failed_requests++;
            continue;
        }
        g_success_requests++;
        
        // GET操作
        g_total_requests++;
        auto get_result = co_await session.get(key);
        if (!get_result || get_result->empty()) {
            g_failed_requests++;
            continue;
        }
        g_success_requests++;
        
        // DEL操作
        g_total_requests++;
        auto del_result = co_await session.del(key);
        if (!del_result || del_result->empty()) {
            g_failed_requests++;
            continue;
        }
        g_success_requests++;
        
        request_count++;
    }
    
    // 关闭连接
    auto close_result = co_await session.close();
    if (!close_result) {
        std::cerr << "[Thread-" << thread_id << "][Session-" << session_id 
                  << "] Failed to close: " << close_result.error().message() << std::endl;
    }
    
    // 减少活跃会话计数
    active_sessions--;
    
    co_return nil();
}

// 单个线程的压测函数
void benchmarkThread(int thread_id, const BenchmarkConfig& config, std::atomic<int>& active_sessions) {
    try {
        // 每个线程创建自己的运行时和调度器
        RuntimeBuilder builder;
        Runtime runtime = builder.build();
        runtime.start();
        
        auto handle = runtime.getCoSchedulerHandle();
        
        // 启动多个会话
        for (int i = 0; i < config.num_sessions_per_thread; ++i) {
            active_sessions++;
            handle.spawn(benchmarkSession(handle, config, thread_id, i, active_sessions));
        }
        
        // 等待所有会话完成
        while (active_sessions > 0 && g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        runtime.stop();
        
    } catch (const std::exception& e) {
        std::cerr << "[Thread-" << thread_id << "] Exception: " << e.what() << std::endl;
    }
}

// 统计输出协程
void printStats(const BenchmarkConfig& config, const std::atomic<int>& active_sessions) {
    auto start_time = std::chrono::steady_clock::now();
    uint64_t last_success = 0;
    
    std::cout << "\n=== Benchmark Statistics ===" << std::endl;
    std::cout << std::setw(10) << "Time(s)" 
              << std::setw(15) << "Total" 
              << std::setw(15) << "Success" 
              << std::setw(15) << "Failed"
              << std::setw(15) << "QPS"
              << std::setw(15) << "Success Rate"
              << std::setw(15) << "Active" << std::endl;
    std::cout << std::string(100, '-') << std::endl;
    
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        
        uint64_t total = g_total_requests.load();
        uint64_t success = g_success_requests.load();
        uint64_t failed = g_failed_requests.load();
        
        uint64_t qps = success - last_success;
        last_success = success;
        
        double success_rate = total > 0 ? (success * 100.0 / total) : 0.0;
        
        std::cout << std::setw(10) << elapsed
                  << std::setw(15) << total
                  << std::setw(15) << success
                  << std::setw(15) << failed
                  << std::setw(15) << qps
                  << std::setw(14) << std::fixed << std::setprecision(2) << success_rate << "%"
                  << std::setw(15) << active_sessions.load()
                  << std::endl;
        
        // 检查是否应该停止
        if (config.use_duration && elapsed >= config.duration_seconds) {
            g_running = false;
            break;
        }
        
        // 如果所有会话都完成了，也停止
        if (!config.use_duration && active_sessions == 0) {
            g_running = false;
            break;
        }
    }
}

// 打印最终统计
void printFinalStats(const std::chrono::steady_clock::time_point& start_time) {
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    double duration_sec = duration / 1000.0;
    
    uint64_t total = g_total_requests.load();
    uint64_t success = g_success_requests.load();
    uint64_t failed = g_failed_requests.load();
    
    double avg_qps = duration_sec > 0 ? (success / duration_sec) : 0.0;
    double success_rate = total > 0 ? (success * 100.0 / total) : 0.0;
    
    std::cout << "\n=== Final Statistics ===" << std::endl;
    std::cout << "Total Duration:    " << std::fixed << std::setprecision(2) << duration_sec << " seconds" << std::endl;
    std::cout << "Total Requests:    " << total << std::endl;
    std::cout << "Success Requests:  " << success << std::endl;
    std::cout << "Failed Requests:   " << failed << std::endl;
    std::cout << "Average QPS:       " << std::fixed << std::setprecision(2) << avg_qps << std::endl;
    std::cout << "Success Rate:      " << std::fixed << std::setprecision(2) << success_rate << "%" << std::endl;
    std::cout << "Avg Latency:       " << std::fixed << std::setprecision(2) 
              << (success > 0 ? (duration_sec * 1000.0 / success) : 0.0) << " ms" << std::endl;
}

int main(int argc, char* argv[]) {
    // 设置日志级别为ERROR，减少输出
    setenv("SPDLOG_LEVEL", "error", 1);
    
    BenchmarkConfig config;
    
    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) {
            config.num_threads = std::stoi(argv[++i]);
        } else if (arg == "--sessions" && i + 1 < argc) {
            config.num_sessions_per_thread = std::stoi(argv[++i]);
        } else if (arg == "--requests" && i + 1 < argc) {
            config.requests_per_session = std::stoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            config.duration_seconds = std::stoi(argv[++i]);
            config.use_duration = true;
        } else if (arg == "--url" && i + 1 < argc) {
            config.redis_url = argv[++i];
        } else if (arg == "--quiet" || arg == "-q") {
            config.quiet = true;
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --threads N      Number of threads (default: 4)\n"
                      << "  --sessions N     Number of sessions per thread (default: 10)\n"
                      << "  --requests N     Number of requests per session (default: 100)\n"
                      << "  --duration N     Run for N seconds (overrides --requests)\n"
                      << "  --url URL        Redis URL (default: redis://:galay123@140.143.142.251:6379)\n"
                      << "  --quiet, -q      Quiet mode, suppress connection logs\n"
                      << "  --help           Show this help message\n";
            return 0;
        }
    }
    
    std::cout << "=== Async Redis Benchmark ===" << std::endl;
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Threads:          " << config.num_threads << std::endl;
    std::cout << "  Sessions/Thread:  " << config.num_sessions_per_thread << std::endl;
    if (config.use_duration) {
        std::cout << "  Duration:         " << config.duration_seconds << " seconds" << std::endl;
    } else {
        std::cout << "  Requests/Session: " << config.requests_per_session << std::endl;
    }
    std::cout << "  Redis URL:        " << config.redis_url << std::endl;
    std::cout << "  Total Sessions:   " << (config.num_threads * config.num_sessions_per_thread) << std::endl;
    std::cout << std::endl;
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 活跃会话计数器
    std::atomic<int> active_sessions{0};
    
    // 启动统计输出线程
    std::thread stats_thread(printStats, config, std::ref(active_sessions));
    
    // 启动压测线程
    std::vector<std::thread> threads;
    for (int i = 0; i < config.num_threads; ++i) {
        threads.emplace_back(benchmarkThread, i, config, std::ref(active_sessions));
    }
    
    // 等待所有压测线程完成
    for (auto& thread : threads) {
        thread.join();
    }
    
    // 停止统计线程
    g_running = false;
    stats_thread.join();
    
    // 打印最终统计
    printFinalStats(start_time);
    
    std::cout << "\nBenchmark completed." << std::endl;
    return 0;
}

