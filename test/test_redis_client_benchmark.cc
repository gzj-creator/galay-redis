#include "galay-redis/async/RedisClient.h"
#include <galay-kernel/kernel/Runtime.h>
#include <iostream>
#include <chrono>
#include <atomic>
#include <vector>

using namespace galay::redis;
using namespace galay::kernel;

// 全局统计
std::atomic<int> success_count{0};
std::atomic<int> error_count{0};
std::atomic<int> timeout_count{0};

/**
 * @brief 单个客户端的性能测试
 */
Coroutine benchmarkClient(IOScheduler* scheduler, int client_id, int operations_per_client)
{
    RedisClient client(scheduler);

    // 连接到Redis服务器
    auto connect_result = co_await client.connect("127.0.0.1", 6379);
    if (!connect_result) {
        std::cerr << "Client " << client_id << " failed to connect: "
                  << connect_result.error().message() << std::endl;
        error_count += operations_per_client;
        co_return;
    }

    std::cout << "Client " << client_id << " connected" << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now();

    // 执行操作
    for (int i = 0; i < operations_per_client; ++i) {
        std::string key = "bench_" + std::to_string(client_id) + "_" + std::to_string(i);
        std::string value = "value_" + std::to_string(i);

        // SET操作
        auto set_result = co_await client.set(key, value).timeout(std::chrono::seconds(5));
        if (set_result && set_result.value()) {
            success_count++;
        } else if (!set_result) {
            if (set_result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
                timeout_count++;
            } else {
                error_count++;
            }
        }

        // GET操作
        auto get_result = co_await client.get(key).timeout(std::chrono::seconds(5));
        if (get_result && get_result.value()) {
            success_count++;
        } else if (!get_result) {
            if (get_result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
                timeout_count++;
            } else {
                error_count++;
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "Client " << client_id << " completed " << (operations_per_client * 2)
              << " operations in " << duration.count() << "ms" << std::endl;

    co_await client.close();
}

/**
 * @brief Pipeline性能测试
 */
Coroutine benchmarkPipeline(IOScheduler* scheduler, int client_id, int batch_size, int batches)
{
    RedisClient client(scheduler);

    auto connect_result = co_await client.connect("127.0.0.1", 6379);
    if (!connect_result) {
        std::cerr << "Pipeline client " << client_id << " failed to connect" << std::endl;
        error_count += batch_size * batches;
        co_return;
    }

    std::cout << "Pipeline client " << client_id << " connected" << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int batch = 0; batch < batches; ++batch) {
        std::vector<std::vector<std::string>> commands;

        // 构建批量命令
        for (int i = 0; i < batch_size; ++i) {
            std::string key = "pipeline_" + std::to_string(client_id) + "_" + std::to_string(batch * batch_size + i);
            std::string value = "value_" + std::to_string(i);
            commands.push_back({"SET", key, value});
        }

        // 执行Pipeline
        auto result = co_await client.pipeline(commands);
        if (result && result.value()) {
            success_count += batch_size;
        } else if (!result) {
            if (result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
                timeout_count += batch_size;
            } else {
                error_count += batch_size;
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "Pipeline client " << client_id << " completed " << (batch_size * batches)
              << " operations in " << duration.count() << "ms" << std::endl;

    co_await client.close();
}

int main(int argc, char* argv[])
{
    // 默认参数
    int num_clients = 10;
    int operations_per_client = 100;
    bool use_pipeline = false;
    int batch_size = 10;

    // 解析命令行参数
    if (argc > 1) num_clients = std::atoi(argv[1]);
    if (argc > 2) operations_per_client = std::atoi(argv[2]);
    if (argc > 3) use_pipeline = (std::string(argv[3]) == "pipeline");
    if (argc > 4) batch_size = std::atoi(argv[4]);

    std::cout << "==================================================" << std::endl;
    std::cout << "Redis Client Performance Benchmark" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "Clients: " << num_clients << std::endl;
    std::cout << "Operations per client: " << operations_per_client << std::endl;
    std::cout << "Mode: " << (use_pipeline ? "Pipeline" : "Normal") << std::endl;
    if (use_pipeline) {
        std::cout << "Batch size: " << batch_size << std::endl;
    }
    std::cout << "Total operations: " << (num_clients * operations_per_client * (use_pipeline ? 1 : 2)) << std::endl;
    std::cout << "==================================================" << std::endl;

    try {
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "Failed to get IO scheduler" << std::endl;
            return 1;
        }

        auto start_time = std::chrono::high_resolution_clock::now();

        // 启动所有客户端
        for (int i = 0; i < num_clients; ++i) {
            if (use_pipeline) {
                int batches = operations_per_client / batch_size;
                scheduler->spawn(benchmarkPipeline(scheduler, i, batch_size, batches));
            } else {
                scheduler->spawn(benchmarkClient(scheduler, i, operations_per_client));
            }
        }

        // 等待所有操作完成
        std::this_thread::sleep_for(std::chrono::seconds(30));

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        runtime.stop();

        // 输出统计结果
        std::cout << "\n==================================================" << std::endl;
        std::cout << "Benchmark Results" << std::endl;
        std::cout << "==================================================" << std::endl;
        std::cout << "Total time: " << duration.count() << "ms" << std::endl;
        std::cout << "Successful operations: " << success_count.load() << std::endl;
        std::cout << "Failed operations: " << error_count.load() << std::endl;
        std::cout << "Timeout operations: " << timeout_count.load() << std::endl;

        int total_ops = success_count.load() + error_count.load() + timeout_count.load();
        if (total_ops > 0) {
            double ops_per_sec = (double)success_count.load() / (duration.count() / 1000.0);
            double success_rate = (double)success_count.load() / total_ops * 100.0;

            std::cout << "Operations per second: " << (int)ops_per_sec << std::endl;
            std::cout << "Success rate: " << success_rate << "%" << std::endl;
        }
        std::cout << "==================================================" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Benchmark failed with exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
