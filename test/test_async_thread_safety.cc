#include <iostream>
#include <memory>
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

// 全局统计
std::atomic<uint64_t> g_total_requests{0};
std::atomic<uint64_t> g_success_requests{0};
std::atomic<uint64_t> g_failed_requests{0};
std::atomic<bool> g_running{true};

std::unique_ptr<AsyncRedisSession> g_session;

// 测试配置
struct ThreadSafetyConfig {
    std::string redis_url = "redis://:galay123@140.143.142.251:6379";
    int num_threads = 8;                // 并发线程数
    int requests_per_thread = 1000;     // 每个线程的请求数
    int duration_seconds = 30;          // 持续时间
    bool use_duration = false;          // 是否使用持续时间模式
    bool use_pipeline = false;          // 是否使用pipeline模式
    int pipeline_batch_size = 10;       // pipeline批量大小
};

// 单个协程的测试 - 共享同一个session
Coroutine<nil> coroutineTestTask(CoSchedulerHandle handle, int coroutine_id, 
                                  const ThreadSafetyConfig& config,
                                  std::atomic<int>& active_coroutines) {
    int request_count = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    std::cout << "[Coroutine-" << coroutine_id << "] Started" << std::endl;
    
    while (true) {
        // 检查是否应该停止
        if (config.use_duration) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            if (elapsed >= config.duration_seconds || !g_running) {
                break;
            }
        } else {
            if (request_count >= config.requests_per_thread) {
                break;
            }
        }
        
        // 生成唯一的key
        std::string key = "thread_safety_c" + std::to_string(coroutine_id) + 
                         "_r" + std::to_string(request_count);
        std::string value = "value_" + std::to_string(request_count);
        
        // SET操作
        g_total_requests++;
        auto set_result = co_await g_session->set(key, value);
        if (!set_result || set_result->empty() || set_result->front().isError()) {
            g_failed_requests++;
            std::cerr << "[Coroutine-" << coroutine_id << "] SET failed: " 
                      << (set_result && !set_result->empty() ? set_result->front().toError() : "unknown error") << std::endl;
            continue;
        }
        g_success_requests++;
        
        // GET操作
        g_total_requests++;
        auto get_result = co_await g_session->get(key);
        if (!get_result || get_result->empty() || get_result->front().isError()) {
            g_failed_requests++;
            std::cerr << "[Coroutine-" << coroutine_id << "] GET failed: " 
                      << (get_result && !get_result->empty() ? get_result->front().toError() : "unknown error") << std::endl;
            continue;
        }
        
        // 验证数据一致性
        if (get_result->front().toString() != value) {
            std::cerr << "[Coroutine-" << coroutine_id << "] Data inconsistency! Expected: " 
                      << value << ", Got: " << get_result->front().toString() << std::endl;
            g_failed_requests++;
            continue;
        }
        g_success_requests++;
        
        // DEL操作
        g_total_requests++;
        auto del_result = co_await g_session->del(key);
        if (!del_result || del_result->empty() || del_result->front().isError()) {
            g_failed_requests++;
            std::cerr << "[Coroutine-" << coroutine_id << "] DEL failed: " 
                      << (del_result && !del_result->empty() ? del_result->front().toError() : "unknown error") << std::endl;
            continue;
        }
        g_success_requests++;
        
        request_count++;
    }
    
    active_coroutines--;
    std::cout << "[Coroutine-" << coroutine_id << "] Completed " << request_count << " iterations" << std::endl;
    
    co_return nil();
}

// 使用Pipeline批量测试的协程
Coroutine<nil> coroutinePipelineTest(CoSchedulerHandle handle, int coroutine_id,
                                      const ThreadSafetyConfig& config,
                                      std::atomic<int>& active_coroutines) {
    int request_count = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    std::cout << "[Pipeline-Coroutine-" << coroutine_id << "] Started" << std::endl;
    
    while (true) {
        // 检查是否应该停止
        if (config.use_duration) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            if (elapsed >= config.duration_seconds || !g_running) {
                break;
            }
        } else {
            if (request_count >= config.requests_per_thread) {
                break;
            }
        }
        
        // 构建一批命令
        std::vector<std::vector<std::string>> commands;
        commands.reserve(config.pipeline_batch_size * 3); // SET + GET + DEL
        
        for (int i = 0; i < config.pipeline_batch_size && request_count < config.requests_per_thread; ++i) {
            std::string key = "pipeline_c" + std::to_string(coroutine_id) + 
                             "_r" + std::to_string(request_count);
            std::string value = "value_" + std::to_string(request_count);
            
            // 添加 SET 命令
            commands.push_back({"SET", key, value});
            // 添加 GET 命令
            commands.push_back({"GET", key});
            // 添加 DEL 命令
            commands.push_back({"DEL", key});
            
            request_count++;
        }
        
        // 执行pipeline
        g_total_requests += commands.size();
        auto pipeline_result = co_await g_session->pipeline(commands);
        
        if (!pipeline_result) {
            g_failed_requests += commands.size();
            std::cerr << "[Pipeline-Coroutine-" << coroutine_id << "] Pipeline failed: " 
                      << pipeline_result.error().message() << std::endl;
            continue;
        }
        
        // 检查结果
        auto& results = pipeline_result.value();
        if (results.size() != commands.size()) {
            std::cerr << "[Pipeline-Coroutine-" << coroutine_id << "] Result size mismatch! Expected: " 
                      << commands.size() << ", Got: " << results.size() << std::endl;
            g_failed_requests += commands.size();
            continue;
        }
        
        // 验证每个结果
        int batch_count = commands.size() / 3;  // 每3个命令一组（SET+GET+DEL）
        for (int i = 0; i < batch_count; ++i) {
            int base_idx = i * 3;
            
            // 检查 SET 结果
            if (results[base_idx].isError()) {
                g_failed_requests++;
                std::cerr << "[Pipeline-Coroutine-" << coroutine_id << "] SET failed in pipeline: " 
                          << results[base_idx].toError() << std::endl;
            } else {
                g_success_requests++;
            }
            
            // 检查 GET 结果
            if (results[base_idx + 1].isError()) {
                g_failed_requests++;
                std::cerr << "[Pipeline-Coroutine-" << coroutine_id << "] GET failed in pipeline: " 
                          << results[base_idx + 1].toError() << std::endl;
            } else {
                // 验证GET返回的值
                std::string expected_value = "value_" + std::to_string(request_count - batch_count + i);
                std::string actual_value = results[base_idx + 1].toString();
                if (actual_value != expected_value) {
                    std::cerr << "[Pipeline-Coroutine-" << coroutine_id << "] GET value mismatch! Expected: " 
                              << expected_value << ", Got: " << actual_value << std::endl;
                    g_failed_requests++;
                } else {
                    g_success_requests++;
                }
            }
            
            // 检查 DEL 结果
            if (results[base_idx + 2].isError()) {
                g_failed_requests++;
                std::cerr << "[Pipeline-Coroutine-" << coroutine_id << "] DEL failed in pipeline: " 
                          << results[base_idx + 2].toError() << std::endl;
            } else {
                g_success_requests++;
            }
        }
    }
    
    active_coroutines--;
    std::cout << "[Pipeline-Coroutine-" << coroutine_id << "] Completed " << request_count << " iterations" << std::endl;
    
    co_return nil();
}

// 统计输出线程
void printStats(const ThreadSafetyConfig& config, const std::atomic<int>& active_coroutines) {
    auto start_time = std::chrono::steady_clock::now();
    uint64_t last_success = 0;
    
    std::cout << "\n=== Concurrency Test Statistics ===" << std::endl;
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
                  << std::setw(15) << active_coroutines.load()
                  << std::endl;
        
        // 检查是否应该停止
        if (config.use_duration && elapsed >= config.duration_seconds) {
            g_running = false;
            break;
        }
        
        // 如果所有协程都完成了，也停止
        if (!config.use_duration && active_coroutines == 0) {
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
    
    if (failed > 0) {
        std::cout << "\n⚠️  WARNING: There were " << failed << " failed requests!" << std::endl;
        std::cout << "This may indicate thread safety issues." << std::endl;
    } else if (success_rate == 100.0) {
        std::cout << "\n✅ SUCCESS: All requests completed successfully!" << std::endl;
        std::cout << "The session appears to be thread-safe under this workload." << std::endl;
    }
}

Coroutine<nil> runTest(std::vector<CoSchedulerHandle>& handles, const ThreadSafetyConfig& config,
                        std::atomic<int>& active_coroutines) {
    std::cout << "Creating shared AsyncRedisSession..." << std::endl;
    
    // 建立连接
    std::cout << "Connecting to Redis server..." << std::endl;
    auto connect_result = co_await g_session->connect(config.redis_url);
    if (!connect_result) {
        std::cerr << "Failed to connect: " << connect_result.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "Connected successfully!" << std::endl;
    
    // 启动多个协程，分布在不同的调度器上
    if (config.use_pipeline) {
        std::cout << "\nStarting " << config.num_threads << " concurrent PIPELINE coroutines (batch size: " 
                  << config.pipeline_batch_size << ") across " << handles.size() << " schedulers..." << std::endl;
        for (int i = 0; i < config.num_threads; ++i) {
            int scheduler_idx = i % handles.size();
            handles[scheduler_idx].spawn(coroutinePipelineTest(handles[scheduler_idx], i, config, active_coroutines));
        }
    } else {
        std::cout << "\nStarting " << config.num_threads << " concurrent coroutines across " 
                  << handles.size() << " schedulers..." << std::endl;
        for (int i = 0; i < config.num_threads; ++i) {
            int scheduler_idx = i % handles.size();
            handles[scheduler_idx].spawn(coroutineTestTask(handles[scheduler_idx], i, config, active_coroutines));
        }
    }
    
    co_return nil();
}


int main(int argc, char* argv[]) {
    // 设置日志级别为ERROR，减少输出
    setenv("SPDLOG_LEVEL", "error", 1);
    
    ThreadSafetyConfig config;
    
    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) {
            config.num_threads = std::stoi(argv[++i]);
        } else if (arg == "--requests" && i + 1 < argc) {
            config.requests_per_thread = std::stoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            config.duration_seconds = std::stoi(argv[++i]);
            config.use_duration = true;
        } else if (arg == "--url" && i + 1 < argc) {
            config.redis_url = argv[++i];
        } else if (arg == "--pipeline") {
            config.use_pipeline = true;
        } else if (arg == "--batch" && i + 1 < argc) {
            config.pipeline_batch_size = std::stoi(argv[++i]);
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --threads N      Number of concurrent coroutines (default: 8)\n"
                      << "  --requests N     Number of requests per coroutine (default: 1000)\n"
                      << "  --duration N     Run for N seconds (overrides --requests)\n"
                      << "  --url URL        Redis URL (default: redis://:galay123@140.143.142.251:6379)\n"
                      << "  --pipeline       Use pipeline mode for batch operations\n"
                      << "  --batch N        Pipeline batch size (default: 10, only with --pipeline)\n"
                      << "  --help           Show this help message\n"
                      << "\nThis test uses a SINGLE AsyncRedisSession shared across multiple coroutines\n"
                      << "in the same scheduler to verify concurrent access handling.\n";
            return 0;
        }
    }
    
    std::cout << "=== AsyncRedisSession " << (config.use_pipeline ? "Pipeline" : "Concurrency") << " Test ===" << std::endl;
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Concurrent Coroutines: " << config.num_threads << std::endl;
    if (config.use_duration) {
        std::cout << "  Duration:              " << config.duration_seconds << " seconds" << std::endl;
    } else {
        std::cout << "  Requests/Coroutine:    " << config.requests_per_thread << std::endl;
        std::cout << "  Total Requests:        " << (config.num_threads * config.requests_per_thread * 3) 
                  << " (SET+GET+DEL)" << std::endl;
    }
    if (config.use_pipeline) {
        std::cout << "  Pipeline Batch Size:   " << config.pipeline_batch_size << std::endl;
    }
    std::cout << "  Redis URL:             " << config.redis_url << std::endl;
    std::cout << "\n📝 NOTE: Multiple coroutines share ONE AsyncRedisSession" 
              << (config.use_pipeline ? " using PIPELINE mode." : " in NORMAL mode.") << std::endl;
    std::cout << std::endl;
    
    try {
        // 创建运行时和调度器
        RuntimeBuilder builder;
        Runtime runtime = builder.build();
        runtime.start();
        
        // 获取多个调度器句柄（用于多线程测试）
        std::vector<CoSchedulerHandle> handles;
        int num_schedulers = std::min(config.num_threads, 4); // 最多使用4个调度器
        for (int i = 0; i < num_schedulers; ++i) {
            auto handle_opt = runtime.getCoSchedulerHandle(i);
            if (handle_opt) {
                handles.push_back(handle_opt.value());
            } else {
                std::cerr << "Failed to get scheduler handle " << i << std::endl;
                return 1;
            }
        }
        
        std::cout << "Using " << num_schedulers << " schedulers for testing" << std::endl;
        
        // 创建共享的AsyncRedisSession（使用第一个调度器）
        g_session = std::make_unique<AsyncRedisSession>(handles[0]);
        
        auto start_time = std::chrono::steady_clock::now();
        
        // 活跃协程计数器（必须在外部创建，以便统计线程可以访问）
        std::atomic<int> active_coroutines{config.num_threads};
        
        // 启动统计输出线程
        std::thread stats_thread(printStats, config, std::ref(active_coroutines));
        
        // 启动测试协程
        handles[0].spawn(runTest(handles, config, active_coroutines));
        
        // 等待测试完成
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // 停止统计线程
        stats_thread.join();
        
        // 打印最终统计
        printFinalStats(start_time);
        
        // 关闭连接
        std::cout << "\nClosing connection..." << std::endl;
        // 注意：这里不能用 co_await，所以我们让 session 自动析构
        g_session.reset();
        
        // 等待一小段时间让所有协程清理完成
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        runtime.stop();
        
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\nThread safety test completed." << std::endl;
    return 0;
}

